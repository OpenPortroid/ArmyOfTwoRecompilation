// mouse_aim.h - Native mouse aiming (see mouse_aim.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::ui
{
  class Window;
}

namespace ao2::mouse
{
  // Starts taking mouse movement from the window (UI thread).
  void Attach(rex::ui::Window *window);

  // Adds movement in mouse counts as if the mouse moved (scripted input).
  void InjectDelta(float dx, float dy);

  // Local camera prediction: the mouse turn of `controller` (rotation units)
  // that the drawn view should add because the simulation has not applied
  // it yet. `step` / `alpha` / `interpolated`: the drawn view's step and how
  // far it is placed past the previous one (interp_camera.cpp). False when
  // there is nothing to add (also during cutscenes).
  bool PredictedTurn(const uint8_t *base, uint32_t controller, uint32_t step, double alpha, bool interpolated,
                     float &pitch, float &yaw);
  // The view of `controller` on simulation step `step` (rotation units): on
  // a turret, whether it is on the gun yet, for the prediction
  // (interp_camera.cpp; game thread).
  void NoteView(const uint8_t *base, uint32_t controller, uint32_t step, float pitch, float yaw);

  // Online co-op: this machine's player (0 while not known), and a turn of
  // its view towards a point `yaw` / `pitch` rotation units away, at most
  // `max_step` per call - sent through the lockstep input like the mouse, so
  // both machines apply it. Turns already on their way count as done. Game
  // thread (the autopilot's aimbot).
  uint32_t LockstepController();
  void TurnTowards(float yaw, float pitch, float max_step);

  // This machine's player is driving a vehicle (at the wheel of the
  // hovercraft - not on a turret, not a vehicle's gunner). Any thread.
  bool Driving();

  // The game is in the front end: the main menu and its pages, the lobby
  // (the Shell world). Any thread.
  bool InFrontEnd();

  // The pawn of the player of pad `user` when aim (the left trigger) aims or
  // zooms: in play (the game turns its view - not the menus, the lobby, the
  // pause menu or a cutscene), on foot, on a turret or at a vehicle's gun;
  // 0 otherwise, also at the wheel (the triggers are the gas and the brake).
  // Online co-op: this machine's player. Game thread.
  uint32_t AimTogglePawn(PPCContext &ctx, uint8_t *base, uint32_t user);
  // The pawn's weapon (Pawn.Weapon) is a grenade (an AO2Grenade). Game thread.
  bool HoldsGrenade(const uint8_t *base, uint32_t pawn);
  // For the log (ao2_mouse_debug): the trigger as polled, and aiming latched.
  void NoteToggleAim(uint32_t user, uint8_t trigger, bool on);

  // The guest user the keyboard and mouse belong to (rexruntime.dll's
  // rex_input_keyboard_user): 0 unless split-screen gives them to another
  // player, 0xFFFFFFFF when nobody has them; 0 from a runtime without it.
  uint32_t KeyboardUser();

  // Right stick from the mouse while native aiming is idle (vehicles, menus).
  // False when there is nothing to add.
  bool TakeStickFallback(int32_t &rx, int32_t &ry);
} // namespace ao2::mouse
