// debug_menu.h - The F1 debug menu (see debug_menu.cpp).
#pragma once

#include <filesystem>
#include <functional>
#include <string>

struct ImFontAtlas;

namespace rex::input
{
  class InputSystem;
}
namespace rex::ui
{
  class ImGuiDrawer;
  class ImmediateDrawer;
  class Window;
} // namespace rex::ui

namespace ao2::menu
{
  // UI thread, once the runtime is set up. `sdk_drawer` is the SDK's ImGui
  // drawer when the Xenos presenter draws the overlays (`sdk_immediate` its
  // immediate drawer); null with the native renderer, where the menu brings
  // its own drawer (gpu/native/overlay.h).
  // Checkpoints: the disc's under `game_data_root`, the DLC's in
  // ao2::dlc::Dir().
  void Attach(rex::ui::Window *window, rex::ui::ImGuiDrawer *sdk_drawer, rex::ui::ImmediateDrawer *sdk_immediate,
              rex::input::InputSystem *input, const std::filesystem::path &game_data_root);

  // Any thread: the menu is open (the game gets no input).
  bool IsOpen();

  // ImGui font setup (ReXApp::OnConfigureFonts): a readable UI font with
  // Cyrillic.
  void ConfigureFonts(ImFontAtlas *atlas);

  // Any thread: a window of the port's own (invites, coop_invite.cpp) on the
  // menu's drawer. `draw` runs on the UI thread inside an ImGui frame and
  // returns false to close the window. Modal: the game gets no input while
  // it is open (like the debug menu); Esc closes it unless own_escape (the
  // window handles Esc itself, e.g. to animate its closing). Opening a window
  // with the same id again replaces it.
  void OpenWindow(const std::string &id, std::function<bool()> draw, bool own_escape = false);
  // Any thread: closes that window if it is open.
  void CloseWindow(const std::string &id);

  // Any thread: a notice drawn over the picture that takes no input (with
  // the native renderer in the frame rate overlay's layer). `draw` runs on
  // the UI thread inside an ImGui frame and returns false when the notice
  // is over; null clears it.
  void SetNotice(std::function<bool()> draw);
  // Any thread: the same in a slot of its own, drawn after the notice (the
  // achievement toast, achievements.cpp), so neither replaces the other.
  void SetToast(std::function<bool()> draw);
  // UI thread: the immediate drawer of the notices' ImGui drawer, for their
  // textures (null before Attach).
  rex::ui::ImmediateDrawer *NoticeImmediateDrawer();
} // namespace ao2::menu
