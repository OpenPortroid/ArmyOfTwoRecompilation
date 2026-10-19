// graphics_menu.h - The game's Options screen gets a Graphics page and, from that, a Controls
// page (see graphics_menu.cpp).
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

  // The Controls page waits for a key (or has just got one and
  // waits for the keys to go up): the menus' mouse and keys keep off.
  bool Capturing();

  // The pause menu's Options row "Graphics and controls": an option button
  // that acts as a plain button (A opens the Graphics page).
  bool IsOpenButton(const uint8_t *base, uint32_t widget);

  // Every pad poll (XInputGetState) of `user`, after the menus' mouse: true
  // when the page takes the input and the state was emptied. Game thread.
  bool OnPadPoll(PPCContext &ctx, uint8_t *base, uint32_t user, uint32_t state);
} // namespace ao2::gfxmenu
