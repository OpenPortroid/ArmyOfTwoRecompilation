// overlay.h - Dear ImGui over the native renderer's picture.
//
// The SDK's ImGui drawer (rex::ui::ImGuiDrawer) renders through a
// ui::ImmediateDrawer. With the Xenos presenter gone there is none, so the
// native renderer brings its own (CreateImmediateDrawer): ImGui still runs on
// the UI thread, the drawer records its vertices, indices and draws into a
// CPU frame, and the renderer draws the latest recorded frame over the guest
// picture at each present. Each present also asks the app for the next UI
// frame (SetFrameRequest), so the overlay updates at the game's frame rate.
// Each drawer records into a layer of its own; the layers are drawn in order
// (the frame rate overlay under the debug menu).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/ui/immediate_drawer.h>

namespace ao2::gpu::overlay
{
  struct Draw
  {
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    int32_t base_vertex = 0;
    uint64_t texture = 0;
    float clip[4] = {}; // left, top, right, bottom in the frame's coordinates
  };

  struct Texture
  {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // R8G8B8A8, tightly packed
  };

  struct Frame
  {
    float width = 0.0f, height = 0.0f; // coordinate space (the whole window)
    std::vector<rex::ui::ImmediateVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<Draw> draws;
  };

  using TextureMap = std::unordered_map<uint64_t, std::shared_ptr<const Texture>>;

  enum Layer : uint32_t
  {
    kPerfLayer, // perf_overlay.h
    kMenuLayer, // debug_menu.h
    kLayerCount,
  };

  // The drawer for an ImGuiDrawer (UI thread).
  std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer(Layer layer);

  // Called at each present (GPU thread); must be cheap and not block.
  void SetFrameRequest(void (*request)());
  // Nothing to draw in `layer` until its next recorded frame.
  void Clear(Layer layer);

  // Renderer, once per present: asks for the next UI frame, then visits the
  // latest one of each layer, in order, with the live textures (under the
  // overlay lock).
  void RequestFrame();
  void Visit(const std::function<void(const Frame &, const TextureMap &)> &visitor);
} // namespace ao2::gpu::overlay
