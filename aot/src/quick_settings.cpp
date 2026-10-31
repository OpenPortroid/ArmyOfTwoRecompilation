// quick_settings.cpp - The F3 quick settings window: field of view and mouse
// sensitivity, changed live (fov.cpp, mouse_aim.cpp).
//
// A port window of the debug menu's drawer (debug_menu.h OpenWindow): the
// game gets no input while it is open. The values last until the game is
// closed; settings/*.toml keeps what the game starts with.
#include "quick_settings.h"

#include "debug_menu.h"
#include "ui_text.h"

#include <algorithm>
#include <atomic>

#include <imgui.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(double, ao2_fov);
REXCVAR_DECLARE(double, ao2_fov_aim);
REXCVAR_DECLARE(double, ao2_mouse_sensitivity);
REXCVAR_DECLARE(double, ao2_mouse_zoom_sensitivity);

REXCVAR_DEFINE_STRING(ao2_quick_settings_key, "F3", "General",
                      "Key that opens the quick settings (field of view, mouse sensitivity); empty = none");

namespace
{
  using ao2::text::Tr;

  constexpr const char *kWindowId = "quick_settings";
  std::atomic<bool> g_open{false};

  // A slider for a double cvar; true when it changed.
  bool Slider(const char *label, double &value, double min, double max, const char *format)
  {
    float v = float(value);
    ImGui::SetNextItemWidth(260.0f);
    if (!ImGui::SliderFloat(label, &v, float(min), float(max), format, ImGuiSliderFlags_AlwaysClamp))
    {
      return false;
    }
    value = std::clamp(double(v), min, max);
    return true;
  }

  bool Draw()
  {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    std::string title = std::string(Tr("Quick settings")) + "###quick_settings";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
    {
      ImGui::SeparatorText(Tr("Field of view"));
      double fov = REXCVAR_GET(ao2_fov);
      if (Slider(Tr("On foot"), fov, 40.0, 130.0, "%.0f\xC2\xB0"))
      {
        REXCVAR_SET(ao2_fov, fov);
      }
      double aim = REXCVAR_GET(ao2_fov_aim);
      if (Slider(Tr("Aiming zoom"), aim, 0.0, 1.0, "%.2f"))
      {
        REXCVAR_SET(ao2_fov_aim, aim);
      }
      ImGui::TextDisabled("%s", Tr("Game: 70 deg on foot, aiming zoom 1 (40 deg)."));
      ImGui::TextDisabled("%s", Tr("0 = aiming does not narrow the view. Not the sniper scope."));
      if (ImGui::Button(Tr("Game's field of view")))
      {
        REXCVAR_SET(ao2_fov, 70.0);
        REXCVAR_SET(ao2_fov_aim, 1.0);
      }

      ImGui::SeparatorText(Tr("Mouse"));
      double sens = REXCVAR_GET(ao2_mouse_sensitivity);
      if (Slider(Tr("Sensitivity"), sens, 0.1, 10.0, "%.2f"))
      {
        REXCVAR_SET(ao2_mouse_sensitivity, sens);
      }
      double zoom = REXCVAR_GET(ao2_mouse_zoom_sensitivity);
      if (Slider(Tr("Zoomed sensitivity"), zoom, 0.1, 3.0, "%.2f"))
      {
        REXCVAR_SET(ao2_mouse_zoom_sensitivity, zoom);
      }

      ImGui::Spacing();
      std::string key = ao2::quick::Key();
      ImGui::TextDisabled(Tr("Until the game is closed. %s or Esc closes this."), key.empty() ? "-" : key.c_str());
    }
    ImGui::End();
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
      g_open = false;
      return false;
    }
    return true;
  }
} // namespace

namespace ao2::quick
{
  std::string Key()
  {
    return REXCVAR_GET(ao2_quick_settings_key);
  }

  void Toggle()
  {
    if (g_open.exchange(false))
    {
      ao2::menu::CloseWindow(kWindowId);
      return;
    }
    g_open = true;
    ao2::menu::OpenWindow(kWindowId, Draw, true);
  }
} // namespace ao2::quick
