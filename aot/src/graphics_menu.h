// graphics_menu.h - The game's Options screen gets a Graphics page (see graphics_menu.cpp).
#pragma once

namespace rex::ui
{
  class Window;
}

namespace ao2::gfxmenu
{
  // The window "Display mode" switches between fullscreen and windowed.
  void Attach(rex::ui::Window *window);
} // namespace ao2::gfxmenu
