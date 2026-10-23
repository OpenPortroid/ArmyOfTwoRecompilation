// command_recorder.h - the renderer's D3D12 command list, recorded on the GPU
// command thread and replayed on a thread of its own.
//
// The command thread emulates the guest GPU (state, EDRAM, textures, guest
// memory) and used to call D3D12 itself: about half of its busy time was in
// the runtime and the driver (state changes, barriers, draws, submits). In
// deferred mode every call is appended to a stream instead and a replay
// thread makes it on the real command list, in order, together with what must
// stay in order with the list: submits and fence signals on the queue,
// command allocator resets, render target and depth views (D3D12 reads those
// descriptors when the command is recorded, and the renderer rewrites them
// in place when render targets trade resources) and presents.
//
// What the command thread gives the stream must stay valid until the GPU is
// done with it, as with a command list recorded directly: resources are
// released through DeferRelease, upload memory and shader-visible descriptor
// slots are reused after the frame's fence. A fence value is signalled only
// once the replay thread got to its Signal, so waiting for the fence also
// waits for the replay (Signal sends the stream on at once).
//
// Without deferred mode every call goes straight to the list.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ao2::gpu
{
  class CommandRecorder
  {
  public:
    using Callback = void (*)(uint64_t a, uint64_t b);
    using ErrorLog = void (*)(const char *what, HRESULT hr);

    // At exit the replay thread is left alone (it may be blocked in a
    // present that never returns).
    ~CommandRecorder()
    {
      if (thread_.joinable())
      {
        thread_.detach();
      }
    }

    // Creates the real list (open, on `allocator`).
    HRESULT Init(ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12CommandAllocator *allocator)
    {
      device_ = device;
      queue_ = queue;
      return device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list_));
    }

    // Starts or stops deferred mode (waits until everything recorded so far
    // was replayed). `log` reports failed Close/Reset on the replay thread.
    void SetDeferred(bool on, ErrorLog log)
    {
      if (on == deferred_)
      {
        return;
      }
      Drain();
      log_ = log;
      if (on && !thread_.joinable())
      {
        stop_ = false;
        thread_ = std::thread([this] { Run(); });
        SetThreadDescription(thread_.native_handle(), L"Native GPU record");
      }
      deferred_ = on;
    }

    bool deferred() const { return deferred_; }

    void Stop()
    {
      if (thread_.joinable())
      {
        Drain();
        {
          std::lock_guard lock(mutex_);
          stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
      }
      deferred_ = false;
    }

    // Commands recorded so far (positions to wait for).
    uint64_t Position() const { return recorded_; }

    // Waits until the replay thread has made everything up to `position`.
    void WaitReplayed(uint64_t position)
    {
      if (!deferred_)
      {
        return;
      }
      Send();
      std::unique_lock lock(mutex_);
      replayed_cv_.wait(lock, [&] { return replayed_ >= position; });
    }

    void Drain() { WaitReplayed(recorded_); }

    // Hands what was recorded to the replay thread now (also done on its own
    // every few kilobytes, on submits and signals).
    void Send()
    {
      if (!block_ || block_->data.empty())
      {
        return;
      }
      {
        std::lock_guard lock(mutex_);
        queue_blocks_.push_back(block_);
      }
      block_ = nullptr;
      wake_.notify_one();
    }

    // Compatibility with the ComPtr it replaces: g.list->X(), g.list.Get().
    CommandRecorder *operator->() { return this; }
    CommandRecorder *Get() { return this; }

    // ---- queue, allocator, device: in order with the list

    // Executes the (closed) list on the queue.
    void ExecuteOnQueue()
    {
      if (!deferred_)
      {
        ID3D12CommandList *lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        return;
      }
      Put(kExecute, nullptr, 0);
    }

    void Signal(ID3D12Fence *fence, uint64_t value)
    {
      if (!deferred_)
      {
        queue_->Signal(fence, value);
        return;
      }
      struct
      {
        ID3D12Fence *fence;
        uint64_t value;
      } a = {fence, value};
      Put(kSignal, &a, sizeof(a));
      Send();
    }

    void ResetAllocator(ID3D12CommandAllocator *allocator)
    {
      if (!deferred_)
      {
        HRESULT hr = allocator->Reset();
        if (FAILED(hr) && log_)
        {
          log_("command allocator Reset", hr);
        }
        return;
      }
      Put(kResetAllocator, &allocator, sizeof(allocator));
    }

    void CreateRtv(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                   D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
      if (!deferred_)
      {
        device_->CreateRenderTargetView(resource, desc, handle);
        return;
      }
      struct
      {
        ID3D12Resource *resource;
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        uint32_t has_desc;
        D3D12_RENDER_TARGET_VIEW_DESC desc;
      } a = {resource, handle, desc ? 1u : 0u, desc ? *desc : D3D12_RENDER_TARGET_VIEW_DESC{}};
      Put(kCreateRtv, &a, sizeof(a));
    }

    void CreateDsv(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                   D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
      if (!deferred_)
      {
        device_->CreateDepthStencilView(resource, desc, handle);
        return;
      }
      struct
      {
        ID3D12Resource *resource;
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        uint32_t has_desc;
        D3D12_DEPTH_STENCIL_VIEW_DESC desc;
      } a = {resource, handle, desc ? 1u : 0u, desc ? *desc : D3D12_DEPTH_STENCIL_VIEW_DESC{}};
      Put(kCreateDsv, &a, sizeof(a));
    }

    // Shader-visible descriptors of the frame's tables: read by the GPU only,
    // so they can be written on the replay thread as well.
    void CreateSrv(ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                   D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
      if (!deferred_)
      {
        device_->CreateShaderResourceView(resource, desc, handle);
        return;
      }
      struct
      {
        ID3D12Resource *resource;
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
        uint32_t has_desc;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc;
      } a = {resource, handle, desc ? 1u : 0u, desc ? *desc : D3D12_SHADER_RESOURCE_VIEW_DESC{}};
      Put(kCreateSrv, &a, sizeof(a));
    }

    // Copies a descriptor of a heap that is not shader-visible (and not
    // written through the stream).
    void CopyDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                        D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
      if (!deferred_)
      {
        device_->CopyDescriptorsSimple(1, dst, src, type);
        return;
      }
      struct
      {
        D3D12_CPU_DESCRIPTOR_HANDLE dst, src;
        uint32_t type;
      } a = {dst, src, uint32_t(type)};
      Put(kCopyDescriptor, &a, sizeof(a));
    }

    // `callback(a, b)` on the replay thread after what was recorded before
    // (at once without deferred mode).
    void Call(Callback callback, uint64_t a, uint64_t b)
    {
      if (!deferred_)
      {
        callback(a, b);
        return;
      }
      struct
      {
        Callback callback;
        uint64_t a, b;
      } c = {callback, a, b};
      Put(kCall, &c, sizeof(c));
      Send();
    }

    // ---- ID3D12GraphicsCommandList (the part the renderer uses)

    HRESULT Close()
    {
      if (!deferred_)
      {
        return list_->Close();
      }
      Put(kClose, nullptr, 0);
      return S_OK;
    }

    HRESULT Reset(ID3D12CommandAllocator *allocator, ID3D12PipelineState *pso)
    {
      if (!deferred_)
      {
        return list_->Reset(allocator, pso);
      }
      struct
      {
        ID3D12CommandAllocator *allocator;
        ID3D12PipelineState *pso;
      } a = {allocator, pso};
      Put(kReset, &a, sizeof(a));
      return S_OK;
    }

    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap *const *heaps)
    {
      if (!deferred_)
      {
        return list_->SetDescriptorHeaps(count, heaps);
      }
      PutArray(kSetDescriptorHeaps, count, heaps, count * sizeof(heaps[0]));
    }

    void SetGraphicsRootSignature(ID3D12RootSignature *root)
    {
      if (!deferred_)
      {
        return list_->SetGraphicsRootSignature(root);
      }
      Put(kSetRootSignature, &root, sizeof(root));
    }

    void SetPipelineState(ID3D12PipelineState *pso)
    {
      if (!deferred_)
      {
        return list_->SetPipelineState(pso);
      }
      Put(kSetPipelineState, &pso, sizeof(pso));
    }

    void SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table)
    {
      if (!deferred_)
      {
        return list_->SetGraphicsRootDescriptorTable(index, table);
      }
      RootArg a = {index, table.ptr};
      Put(kSetRootTable, &a, sizeof(a));
    }

    void SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
    {
      if (!deferred_)
      {
        return list_->SetGraphicsRootConstantBufferView(index, address);
      }
      RootArg a = {index, address};
      Put(kSetRootCbv, &a, sizeof(a));
    }

    void SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
    {
      if (!deferred_)
      {
        return list_->SetGraphicsRootShaderResourceView(index, address);
      }
      RootArg a = {index, address};
      Put(kSetRootSrv, &a, sizeof(a));
    }

    void SetGraphicsRoot32BitConstants(UINT index, UINT count, const void *values, UINT offset)
    {
      if (!deferred_)
      {
        return list_->SetGraphicsRoot32BitConstants(index, count, values, offset);
      }
      uint32_t head[3] = {index, count, offset};
      uint8_t *p = Reserve(kSetRoot32, sizeof(head) + count * 4);
      std::memcpy(p, head, sizeof(head));
      std::memcpy(p + sizeof(head), values, count * 4);
    }

    void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
    {
      if (!deferred_)
      {
        return list_->IASetPrimitiveTopology(topology);
      }
      Put(kSetTopology, &topology, sizeof(topology));
    }

    void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view)
    {
      if (!deferred_)
      {
        return list_->IASetIndexBuffer(view);
      }
      PutArray(kSetIndexBuffer, view ? 1 : 0, view, view ? sizeof(*view) : 0);
    }

    void IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW *views)
    {
      if (!deferred_)
      {
        return list_->IASetVertexBuffers(start, count, views);
      }
      uint32_t head[2] = {start, views ? count : 0};
      uint8_t *p = Reserve(kSetVertexBuffers, sizeof(head) + head[1] * sizeof(views[0]));
      std::memcpy(p, head, sizeof(head));
      if (head[1])
      {
        std::memcpy(p + sizeof(head), views, head[1] * sizeof(views[0]));
      }
    }

    void RSSetViewports(UINT count, const D3D12_VIEWPORT *viewports)
    {
      if (!deferred_)
      {
        return list_->RSSetViewports(count, viewports);
      }
      PutArray(kSetViewports, count, viewports, count * sizeof(viewports[0]));
    }

    void RSSetScissorRects(UINT count, const D3D12_RECT *rects)
    {
      if (!deferred_)
      {
        return list_->RSSetScissorRects(count, rects);
      }
      PutArray(kSetScissors, count, rects, count * sizeof(rects[0]));
    }

    void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs, BOOL single,
                            const D3D12_CPU_DESCRIPTOR_HANDLE *dsv)
    {
      if (!deferred_)
      {
        return list_->OMSetRenderTargets(count, rtvs, single, dsv);
      }
      // A single range is passed as its first handle and expanded here.
      struct
      {
        uint32_t count, has_dsv;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
      } a = {};
      a.count = rtvs ? count : 0;
      a.has_dsv = dsv ? 1 : 0;
      if (dsv)
      {
        a.dsv = *dsv;
      }
      for (uint32_t i = 0; i < a.count && i < 8; ++i)
      {
        a.rtvs[i] = single ? D3D12_CPU_DESCRIPTOR_HANDLE{rtvs[0].ptr + i * rtv_size_} : rtvs[i];
      }
      Put(kSetRenderTargets, &a, sizeof(a));
    }

    void OMSetBlendFactor(const FLOAT factor[4])
    {
      if (!deferred_)
      {
        return list_->OMSetBlendFactor(factor);
      }
      float f[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      if (factor)
      {
        std::memcpy(f, factor, sizeof(f));
      }
      Put(kSetBlendFactor, f, sizeof(f));
    }

    void OMSetStencilRef(UINT ref)
    {
      if (!deferred_)
      {
        return list_->OMSetStencilRef(ref);
      }
      Put(kSetStencilRef, &ref, sizeof(ref));
    }

    void DrawInstanced(UINT vertices, UINT instances, UINT start_vertex, UINT start_instance)
    {
      if (!deferred_)
      {
        return list_->DrawInstanced(vertices, instances, start_vertex, start_instance);
      }
      uint32_t a[4] = {vertices, instances, start_vertex, start_instance};
      Put(kDraw, a, sizeof(a));
    }

    void DrawIndexedInstanced(UINT indices, UINT instances, UINT start_index, INT base_vertex, UINT start_instance)
    {
      if (!deferred_)
      {
        return list_->DrawIndexedInstanced(indices, instances, start_index, base_vertex, start_instance);
      }
      uint32_t a[5] = {indices, instances, start_index, uint32_t(base_vertex), start_instance};
      Put(kDrawIndexed, a, sizeof(a));
    }

    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers)
    {
      if (!deferred_)
      {
        return list_->ResourceBarrier(count, barriers);
      }
      PutArray(kBarrier, count, barriers, count * sizeof(barriers[0]));
    }

    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT count,
                               const D3D12_RECT *rects)
    {
      if (!deferred_)
      {
        return list_->ClearRenderTargetView(view, color, count, rects);
      }
      struct Head
      {
        D3D12_CPU_DESCRIPTOR_HANDLE view;
        float color[4];
        uint32_t count;
      } head = {view, {color[0], color[1], color[2], color[3]}, rects ? count : 0};
      uint8_t *p = Reserve(kClearRtv, sizeof(head) + head.count * sizeof(D3D12_RECT));
      std::memcpy(p, &head, sizeof(head));
      if (head.count)
      {
        std::memcpy(p + sizeof(head), rects, head.count * sizeof(D3D12_RECT));
      }
    }

    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                               UINT count, const D3D12_RECT *rects)
    {
      if (!deferred_)
      {
        return list_->ClearDepthStencilView(view, flags, depth, stencil, count, rects);
      }
      struct Head
      {
        D3D12_CPU_DESCRIPTOR_HANDLE view;
        uint32_t flags;
        float depth;
        uint32_t stencil;
        uint32_t count;
      } head = {view, uint32_t(flags), depth, stencil, rects ? count : 0};
      uint8_t *p = Reserve(kClearDsv, sizeof(head) + head.count * sizeof(D3D12_RECT));
      std::memcpy(p, &head, sizeof(head));
      if (head.count)
      {
        std::memcpy(p + sizeof(head), rects, head.count * sizeof(D3D12_RECT));
      }
    }

    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
                           const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
    {
      if (!deferred_)
      {
        return list_->CopyTextureRegion(dst, x, y, z, src, box);
      }
      struct
      {
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        uint32_t x, y, z, has_box;
        D3D12_BOX box;
      } a = {*dst, *src, x, y, z, box ? 1u : 0u, box ? *box : D3D12_BOX{}};
      Put(kCopyTexture, &a, sizeof(a));
    }

    void CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset,
                          UINT64 bytes)
    {
      if (!deferred_)
      {
        return list_->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes);
      }
      struct
      {
        ID3D12Resource *dst;
        uint64_t dst_offset;
        ID3D12Resource *src;
        uint64_t src_offset, bytes;
      } a = {dst, dst_offset, src, src_offset, bytes};
      Put(kCopyBuffer, &a, sizeof(a));
    }

    void CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
    {
      if (!deferred_)
      {
        return list_->CopyResource(dst, src);
      }
      ID3D12Resource *a[2] = {dst, src};
      Put(kCopyResource, a, sizeof(a));
    }

    void ResolveSubresource(ID3D12Resource *dst, UINT dst_sub, ID3D12Resource *src, UINT src_sub, DXGI_FORMAT format)
    {
      if (!deferred_)
      {
        return list_->ResolveSubresource(dst, dst_sub, src, src_sub, format);
      }
      struct
      {
        ID3D12Resource *dst, *src;
        uint32_t dst_sub, src_sub, format;
      } a = {dst, src, dst_sub, src_sub, uint32_t(format)};
      Put(kResolveSubresource, &a, sizeof(a));
    }

    void BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
    {
      if (!deferred_)
      {
        return list_->BeginQuery(heap, type, index);
      }
      QueryArg a = {heap, uint32_t(type), index};
      Put(kBeginQuery, &a, sizeof(a));
    }

    void EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
    {
      if (!deferred_)
      {
        return list_->EndQuery(heap, type, index);
      }
      QueryArg a = {heap, uint32_t(type), index};
      Put(kEndQuery, &a, sizeof(a));
    }

    void ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
                          ID3D12Resource *dst, UINT64 offset)
    {
      if (!deferred_)
      {
        return list_->ResolveQueryData(heap, type, start, count, dst, offset);
      }
      struct
      {
        ID3D12QueryHeap *heap;
        ID3D12Resource *dst;
        uint64_t offset;
        uint32_t type, start, count;
      } a = {heap, dst, offset, uint32_t(type), start, count};
      Put(kResolveQuery, &a, sizeof(a));
    }

    void set_rtv_size(uint32_t size) { rtv_size_ = size; }

  private:
    enum Op : uint32_t
    {
      kExecute,
      kSignal,
      kResetAllocator,
      kCreateRtv,
      kCreateDsv,
      kCreateSrv,
      kCopyDescriptor,
      kCall,
      kClose,
      kReset,
      kSetDescriptorHeaps,
      kSetRootSignature,
      kSetPipelineState,
      kSetRootTable,
      kSetRootCbv,
      kSetRootSrv,
      kSetRoot32,
      kSetTopology,
      kSetIndexBuffer,
      kSetVertexBuffers,
      kSetViewports,
      kSetScissors,
      kSetRenderTargets,
      kSetBlendFactor,
      kSetStencilRef,
      kDraw,
      kDrawIndexed,
      kBarrier,
      kClearRtv,
      kClearDsv,
      kCopyTexture,
      kCopyBuffer,
      kCopyResource,
      kResolveSubresource,
      kBeginQuery,
      kEndQuery,
      kResolveQuery,
    };

    struct RootArg
    {
      uint32_t index;
      uint64_t value;
    };
    struct QueryArg
    {
      ID3D12QueryHeap *heap;
      uint32_t type, index;
    };
    // Each command: op and payload size, then the payload padded to 8 bytes.
    struct Header
    {
      uint32_t op, size;
    };
    struct Block
    {
      std::vector<uint8_t> data;
      uint64_t commands = 0;
    };

    // A block is sent on when it holds this much (the replay thread starts
    // on a frame's commands while the rest are recorded).
    static constexpr size_t kSendBytes = 16 * 1024;

    uint8_t *Reserve(Op op, size_t size)
    {
      if (!block_)
      {
        block_ = NewBlock();
      }
      size_t padded = (size + 7) & ~size_t(7);
      size_t at = block_->data.size();
      block_->data.resize(at + sizeof(Header) + padded);
      Header h = {uint32_t(op), uint32_t(size)};
      std::memcpy(block_->data.data() + at, &h, sizeof(h));
      ++block_->commands;
      ++recorded_;
      return block_->data.data() + at + sizeof(Header);
    }

    void Put(Op op, const void *payload, size_t size)
    {
      uint8_t *p = Reserve(op, size);
      if (size)
      {
        std::memcpy(p, payload, size);
      }
      if (block_->data.size() >= kSendBytes)
      {
        Send();
      }
    }

    void PutArray(Op op, uint32_t count, const void *items, size_t bytes)
    {
      uint8_t *p = Reserve(op, sizeof(uint32_t) * 2 + bytes);
      uint32_t head[2] = {count, 0};
      std::memcpy(p, head, sizeof(head));
      if (bytes)
      {
        std::memcpy(p + sizeof(head), items, bytes);
      }
      if (block_->data.size() >= kSendBytes)
      {
        Send();
      }
    }

    Block *NewBlock()
    {
      {
        std::lock_guard lock(mutex_);
        if (!free_blocks_.empty())
        {
          Block *b = free_blocks_.back();
          free_blocks_.pop_back();
          return b;
        }
      }
      auto b = std::make_unique<Block>();
      b->data.reserve(kSendBytes * 2);
      Block *raw = b.get();
      std::lock_guard lock(mutex_);
      all_blocks_.push_back(std::move(b));
      return raw;
    }

    void Run()
    {
      for (;;)
      {
        Block *b = nullptr;
        {
          std::unique_lock lock(mutex_);
          wake_.wait(lock, [&] { return stop_ || !queue_blocks_.empty(); });
          if (queue_blocks_.empty())
          {
            return; // stopped with nothing left
          }
          b = queue_blocks_.front();
          queue_blocks_.pop_front();
        }
        Replay(*b);
        uint64_t commands = b->commands;
        b->data.clear();
        b->commands = 0;
        {
          std::lock_guard lock(mutex_);
          free_blocks_.push_back(b);
          replayed_ += commands;
        }
        replayed_cv_.notify_all();
      }
    }

    template <typename T>
    static T Arg(const uint8_t *p)
    {
      T v;
      std::memcpy(&v, p, sizeof(T));
      return v;
    }

    void Replay(const Block &b)
    {
      ID3D12GraphicsCommandList *list = list_.Get();
      const uint8_t *p = b.data.data(), *end = p + b.data.size();
      while (p < end)
      {
        Header h = Arg<Header>(p);
        const uint8_t *a = p + sizeof(Header);
        p = a + ((size_t(h.size) + 7) & ~size_t(7));
        uint32_t count = h.size >= 8 ? Arg<uint32_t>(a) : 0; // PutArray's count
        const uint8_t *items = a + 8;
        switch (Op(h.op))
        {
        case kExecute:
        {
          ID3D12CommandList *lists[] = {list};
          if (list_closed_ok_)
          {
            queue_->ExecuteCommandLists(1, lists);
          }
          break;
        }
        case kSignal:
        {
          struct S
          {
            ID3D12Fence *fence;
            uint64_t value;
          };
          S s = Arg<S>(a);
          queue_->Signal(s.fence, s.value);
          break;
        }
        case kResetAllocator:
        {
          HRESULT hr = Arg<ID3D12CommandAllocator *>(a)->Reset();
          if (FAILED(hr) && log_)
          {
            log_("command allocator Reset", hr);
          }
          break;
        }
        case kCreateRtv:
        {
          struct S
          {
            ID3D12Resource *resource;
            D3D12_CPU_DESCRIPTOR_HANDLE handle;
            uint32_t has_desc;
            D3D12_RENDER_TARGET_VIEW_DESC desc;
          };
          S s = Arg<S>(a);
          device_->CreateRenderTargetView(s.resource, s.has_desc ? &s.desc : nullptr, s.handle);
          break;
        }
        case kCreateDsv:
        {
          struct S
          {
            ID3D12Resource *resource;
            D3D12_CPU_DESCRIPTOR_HANDLE handle;
            uint32_t has_desc;
            D3D12_DEPTH_STENCIL_VIEW_DESC desc;
          };
          S s = Arg<S>(a);
          device_->CreateDepthStencilView(s.resource, s.has_desc ? &s.desc : nullptr, s.handle);
          break;
        }
        case kCreateSrv:
        {
          struct S
          {
            ID3D12Resource *resource;
            D3D12_CPU_DESCRIPTOR_HANDLE handle;
            uint32_t has_desc;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc;
          };
          S s = Arg<S>(a);
          device_->CreateShaderResourceView(s.resource, s.has_desc ? &s.desc : nullptr, s.handle);
          break;
        }
        case kCopyDescriptor:
        {
          struct S
          {
            D3D12_CPU_DESCRIPTOR_HANDLE dst, src;
            uint32_t type;
          };
          S s = Arg<S>(a);
          device_->CopyDescriptorsSimple(1, s.dst, s.src, D3D12_DESCRIPTOR_HEAP_TYPE(s.type));
          break;
        }
        case kCall:
        {
          struct S
          {
            Callback callback;
            uint64_t a, b;
          };
          S s = Arg<S>(a);
          s.callback(s.a, s.b);
          break;
        }
        case kClose:
        {
          HRESULT hr = list->Close();
          list_closed_ok_ = SUCCEEDED(hr);
          if (FAILED(hr) && log_)
          {
            log_("command list Close (the list is dropped)", hr);
          }
          break;
        }
        case kReset:
        {
          struct S
          {
            ID3D12CommandAllocator *allocator;
            ID3D12PipelineState *pso;
          };
          S s = Arg<S>(a);
          HRESULT hr = list->Reset(s.allocator, s.pso);
          if (FAILED(hr) && log_)
          {
            log_("command list Reset", hr);
          }
          break;
        }
        case kSetDescriptorHeaps:
          list->SetDescriptorHeaps(count, reinterpret_cast<ID3D12DescriptorHeap *const *>(items));
          break;
        case kSetRootSignature: list->SetGraphicsRootSignature(Arg<ID3D12RootSignature *>(a)); break;
        case kSetPipelineState: list->SetPipelineState(Arg<ID3D12PipelineState *>(a)); break;
        case kSetRootTable:
        {
          RootArg r = Arg<RootArg>(a);
          list->SetGraphicsRootDescriptorTable(r.index, D3D12_GPU_DESCRIPTOR_HANDLE{r.value});
          break;
        }
        case kSetRootCbv:
        {
          RootArg r = Arg<RootArg>(a);
          list->SetGraphicsRootConstantBufferView(r.index, r.value);
          break;
        }
        case kSetRootSrv:
        {
          RootArg r = Arg<RootArg>(a);
          list->SetGraphicsRootShaderResourceView(r.index, r.value);
          break;
        }
        case kSetRoot32:
        {
          uint32_t head[3];
          std::memcpy(head, a, sizeof(head));
          list->SetGraphicsRoot32BitConstants(head[0], head[1], a + sizeof(head), head[2]);
          break;
        }
        case kSetTopology: list->IASetPrimitiveTopology(Arg<D3D12_PRIMITIVE_TOPOLOGY>(a)); break;
        case kSetIndexBuffer:
          list->IASetIndexBuffer(count ? reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW *>(items) : nullptr);
          break;
        case kSetVertexBuffers:
        {
          uint32_t head[2];
          std::memcpy(head, a, sizeof(head));
          list->IASetVertexBuffers(head[0], head[1],
                                   head[1] ? reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW *>(a + sizeof(head))
                                           : nullptr);
          break;
        }
        case kSetViewports:
          list->RSSetViewports(count, reinterpret_cast<const D3D12_VIEWPORT *>(items));
          break;
        case kSetScissors: list->RSSetScissorRects(count, reinterpret_cast<const D3D12_RECT *>(items)); break;
        case kSetRenderTargets:
        {
          struct S
          {
            uint32_t count, has_dsv;
            D3D12_CPU_DESCRIPTOR_HANDLE dsv;
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
          };
          S s = Arg<S>(a);
          list->OMSetRenderTargets(s.count, s.count ? s.rtvs : nullptr, FALSE, s.has_dsv ? &s.dsv : nullptr);
          break;
        }
        case kSetBlendFactor:
        {
          float f[4];
          std::memcpy(f, a, sizeof(f));
          list->OMSetBlendFactor(f);
          break;
        }
        case kSetStencilRef: list->OMSetStencilRef(Arg<uint32_t>(a)); break;
        case kDraw:
        {
          uint32_t d[4];
          std::memcpy(d, a, sizeof(d));
          list->DrawInstanced(d[0], d[1], d[2], d[3]);
          break;
        }
        case kDrawIndexed:
        {
          uint32_t d[5];
          std::memcpy(d, a, sizeof(d));
          list->DrawIndexedInstanced(d[0], d[1], d[2], INT(d[3]), d[4]);
          break;
        }
        case kBarrier: list->ResourceBarrier(count, reinterpret_cast<const D3D12_RESOURCE_BARRIER *>(items)); break;
        case kClearRtv:
        {
          struct Head
          {
            D3D12_CPU_DESCRIPTOR_HANDLE view;
            float color[4];
            uint32_t count;
          };
          Head head = Arg<Head>(a);
          list->ClearRenderTargetView(head.view, head.color, head.count,
                                      head.count ? reinterpret_cast<const D3D12_RECT *>(a + sizeof(head)) : nullptr);
          break;
        }
        case kClearDsv:
        {
          struct Head
          {
            D3D12_CPU_DESCRIPTOR_HANDLE view;
            uint32_t flags;
            float depth;
            uint32_t stencil;
            uint32_t count;
          };
          Head head = Arg<Head>(a);
          list->ClearDepthStencilView(head.view, D3D12_CLEAR_FLAGS(head.flags), head.depth, UINT8(head.stencil),
                                      head.count,
                                      head.count ? reinterpret_cast<const D3D12_RECT *>(a + sizeof(head)) : nullptr);
          break;
        }
        case kCopyTexture:
        {
          struct S
          {
            D3D12_TEXTURE_COPY_LOCATION dst, src;
            uint32_t x, y, z, has_box;
            D3D12_BOX box;
          };
          S s = Arg<S>(a);
          list->CopyTextureRegion(&s.dst, s.x, s.y, s.z, &s.src, s.has_box ? &s.box : nullptr);
          break;
        }
        case kCopyBuffer:
        {
          struct S
          {
            ID3D12Resource *dst;
            uint64_t dst_offset;
            ID3D12Resource *src;
            uint64_t src_offset, bytes;
          };
          S s = Arg<S>(a);
          list->CopyBufferRegion(s.dst, s.dst_offset, s.src, s.src_offset, s.bytes);
          break;
        }
        case kCopyResource:
        {
          ID3D12Resource *r[2];
          std::memcpy(r, a, sizeof(r));
          list->CopyResource(r[0], r[1]);
          break;
        }
        case kResolveSubresource:
        {
          struct S
          {
            ID3D12Resource *dst, *src;
            uint32_t dst_sub, src_sub, format;
          };
          S s = Arg<S>(a);
          list->ResolveSubresource(s.dst, s.dst_sub, s.src, s.src_sub, DXGI_FORMAT(s.format));
          break;
        }
        case kBeginQuery:
        {
          QueryArg q = Arg<QueryArg>(a);
          list->BeginQuery(q.heap, D3D12_QUERY_TYPE(q.type), q.index);
          break;
        }
        case kEndQuery:
        {
          QueryArg q = Arg<QueryArg>(a);
          list->EndQuery(q.heap, D3D12_QUERY_TYPE(q.type), q.index);
          break;
        }
        case kResolveQuery:
        {
          struct S
          {
            ID3D12QueryHeap *heap;
            ID3D12Resource *dst;
            uint64_t offset;
            uint32_t type, start, count;
          };
          S s = Arg<S>(a);
          list->ResolveQueryData(s.heap, D3D12_QUERY_TYPE(s.type), s.start, s.count, s.dst, s.offset);
          break;
        }
        }
      }
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    ID3D12Device *device_ = nullptr;
    ID3D12CommandQueue *queue_ = nullptr;
    uint32_t rtv_size_ = 0;
    bool deferred_ = false;
    bool list_closed_ok_ = true; // replay thread: the last Close succeeded
    ErrorLog log_ = nullptr;
    uint64_t recorded_ = 0; // command thread

    Block *block_ = nullptr; // being recorded (command thread)
    std::mutex mutex_;
    std::condition_variable wake_, replayed_cv_;
    std::deque<Block *> queue_blocks_;
    std::vector<Block *> free_blocks_;
    std::vector<std::unique_ptr<Block>> all_blocks_;
    uint64_t replayed_ = 0;
    bool stop_ = false;
    std::thread thread_;
  };
} // namespace ao2::gpu
