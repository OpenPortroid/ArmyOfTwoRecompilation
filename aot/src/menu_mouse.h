// menu_mouse.h - The mouse in the game's menus, an Exit item in the main menu (see menu_mouse.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::ui
{
  class Window;
}

namespace ao2::menumouse
{
  // The window the cursor is in, closed by Exit.
  void Attach(rex::ui::Window *window);

  // XInputGetState of player 1 or of the player with the keyboard (`keyboard`)
  // returned the pad state at guest `state` (ctx.r3 = its result): the menus
  // take the mouse and Exit from there. Game thread.
  void OnPadPoll(PPCContext &ctx, uint8_t *base, uint32_t state, bool keyboard);

  // A menu is open: a UI scene taking input is on the stack (in the game it
  // holds none) - the shop (ContactsScene, WeaponSelectUIScene...), the pause
  // menu, the main menu. As of the last pad poll; any thread.
  bool MenuOpen();
} // namespace ao2::menumouse
