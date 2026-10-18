// graphics_menu.h - The game's Options screen gets a Graphics page (see graphics_menu.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::ui
{
  class Window;
}

namespace ao2::gfxmenu
{
  // The window "Display mode" switches between fullscreen and windowed.
  void Attach(rex::ui::Window *window);

  // A label button's caption becomes `english` (ui_text.cpp) in the game's
  // language and fonts. Game thread.
  void SetCaption(PPCContext &ctx, uint8_t *base, uint32_t button, const char *english);
} // namespace ao2::gfxmenu
