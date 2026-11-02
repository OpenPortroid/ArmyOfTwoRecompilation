// overlay.cpp - Dear ImGui over the native renderer's picture (see overlay.h).
#include "overlay.h"

#include <atomic>
#include <mutex>

namespace ao2::gpu::overlay
{
  namespace
  {
    std::mutex g_mutex;
    Frame g_frames[kLayerCount]; // the latest recorded frame of each layer
    TextureMap g_textures;
    std::atomic<uint64_t> g_next_texture{1};
    std::atomic<void (*)()> g_request{nullptr};

    class OverlayTexture final : public rex::ui::ImmediateTexture
    {
    public:
      OverlayTexture(uint32_t width, uint32_t height, uint64_t id) : ImmediateTexture(width, height), id(id) {}
      ~OverlayTexture() override
      {
        // The renderer drops its copy once the id is gone.
        std::lock_guard lock(g_mutex);
        g_textures.erase(id);
      }

      const uint64_t id;
    };

    class Drawer final : public rex::ui::ImmediateDrawer
    {
    public:
      explicit Drawer(Layer layer) : layer_(layer) {}

      // No device is needed: the pixels are kept and uploaded by the renderer
      // when a draw first uses them.
      std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                               rex::ui::ImmediateTextureFilter, bool,
                                                               const uint8_t *data) override
      {
        if (!width || !height || !data)
        {
          return nullptr;
        }
        auto texture = std::make_shared<Texture>();
        texture->width = width;
        texture->height = height;
        texture->rgba.assign(data, data + size_t(width) * height * 4);
        uint64_t id = g_next_texture++;
        {
          std::lock_guard lock(g_mutex);
          g_textures[id] = std::move(texture);
        }
        return std::make_unique<OverlayTexture>(width, height, id);
      }

      void Begin(rex::ui::UIDrawContext &context, float width, float height) override
      {
        ImmediateDrawer::Begin(context, width, height);
        frame_.width = coordinate_space_width();
        frame_.height = coordinate_space_height();
        frame_.vertices.clear();
        frame_.indices.clear();
        frame_.draws.clear();
      }

      void BeginDrawBatch(const rex::ui::ImmediateDrawBatch &batch) override
      {
        batch_vertex_ = uint32_t(frame_.vertices.size());
        batch_index_ = uint32_t(frame_.indices.size());
        batch_indexed_ = batch.indices && batch.index_count > 0;
        if (batch.vertices && batch.vertex_count > 0)
        {
          frame_.vertices.insert(frame_.vertices.end(), batch.vertices, batch.vertices + batch.vertex_count);
        }
        if (batch_indexed_)
        {
          frame_.indices.insert(frame_.indices.end(), batch.indices, batch.indices + batch.index_count);
        }
      }

      void Draw(const rex::ui::ImmediateDraw &draw) override
      {
        // ImGui only draws indexed, textured triangles.
        if (draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles || !draw.texture ||
            draw.count <= 0 || !batch_indexed_)
        {
          return;
        }
        overlay::Draw d;
        d.index_offset = batch_index_ + uint32_t(draw.index_offset);
        d.index_count = uint32_t(draw.count);
        d.base_vertex = int32_t(batch_vertex_) + draw.base_vertex;
        d.texture = static_cast<OverlayTexture *>(draw.texture)->id;
        if (draw.scissor)
        {
          d.clip[0] = draw.scissor_left;
          d.clip[1] = draw.scissor_top;
          d.clip[2] = draw.scissor_right;
          d.clip[3] = draw.scissor_bottom;
        }
        else
        {
          d.clip[2] = frame_.width;
          d.clip[3] = frame_.height;
        }
        frame_.draws.push_back(d);
      }

      void EndDrawBatch() override {}

      void End() override
      {
        {
          std::lock_guard lock(g_mutex);
          std::swap(g_frames[layer_], frame_);
        }
        ImmediateDrawer::End();
      }

    private:
      const Layer layer_;
      Frame frame_;
      uint32_t batch_vertex_ = 0, batch_index_ = 0;
      bool batch_indexed_ = false;
    };
  } // namespace

  std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer(Layer layer)
  {
    return std::make_unique<Drawer>(layer);
  }

  void SetFrameRequest(void (*request)())
  {
    g_request.store(request, std::memory_order_release);
  }

  void Clear(Layer layer)
  {
    std::lock_guard lock(g_mutex);
    Frame &frame = g_frames[layer];
    frame.vertices.clear();
    frame.indices.clear();
    frame.draws.clear();
  }

  void RequestFrame()
  {
    if (auto request = g_request.load(std::memory_order_acquire))
    {
      request();
    }
  }

  void Visit(const std::function<void(const Frame &, const TextureMap &)> &visitor)
  {
    std::lock_guard lock(g_mutex);
    for (const Frame &frame : g_frames)
    {
      visitor(frame, g_textures);
    }
  }
} // namespace ao2::gpu::overlay
