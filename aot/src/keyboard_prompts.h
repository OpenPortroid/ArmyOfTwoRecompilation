// keyboard_prompts.h - Keyboard keys instead of controller buttons wherever
// the game shows a button (see keyboard_prompts.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ao2::prompts
{
  // Whether the game shows keyboard keys (ao2_keyboard_prompts).
  bool Enabled();

  // A Coalesced.<lang> with the controller words taken out ("press and hold
  // left bumper <LB>" -> "hold <LB>"); every .int in it keeps its size.
  // `english`: also the rules for English strings without a PS3 twin. Data
  // that does not parse comes back as it is.
  std::string RewriteLocalization(const std::string &data, bool english, int &rewritten);

  // Game thread, once per frame: keys or the controller's buttons, by what
  // player 1 used last; moves the button characters of the font
  // 00_fonts.Xbox360_18pt to the keys drawn on the replaced page and back.
  void Tick(uint8_t *base);

  // Renderer: changes whenever the pictures switch between the keys and the
  // controller's buttons; the textures below are then looked at again.
  uint32_t Generation();

  // Renderer: split-screen between a keyboard player and a gamepad player.
  // A draw that samples a button picture is then drawn by parts: in `keys`
  // with our picture, in `pad` with the guest's own texture (x0, y0, x1, y1
  // as fractions of the screen); one the players share (across both, or in
  // neither) twice side by side where it was. False when not split so.
  bool SplitActive();
  bool SplitRegions(float keys[4], float pad[4]);
  // Renderer: whether a texture of this shape can be one of the button
  // pictures, keys shown or not.
  bool MayBeButton(uint32_t format, uint32_t width, uint32_t height);

  // Renderer: whether a texture is the button font's page (DXT5 256x256 whose
  // guest data, 64 KB tiled, is the disc's).
  bool IsButtonPage(uint32_t format, uint32_t width, uint32_t height, const uint8_t *guest_data, uint32_t size);

  // Renderer: the replacement of the button page, RGBA8 `size` x `size`
  // (straight alpha), from the page's guest data. False when turned off.
  bool BuildButtonPage(const uint8_t *guest_data, std::vector<uint8_t> &rgba, uint32_t &size);

  // Renderer: the button pictures of the HUD and the menus that are textures
  // of their own (the context action's A, reload's X, ...). The bytes of the
  // base level to look at for a texture of this shape, 0 if none can be one.
  uint32_t ButtonTextureBytes(uint32_t format, uint32_t width, uint32_t height);
  // Which button texture these `bytes` of guest data are (-1 if none).
  int ButtonTextureOf(uint32_t format, uint32_t width, uint32_t height, const uint8_t *guest_data, uint32_t bytes);
  // Its replacement: the bound key's picture where the button was, RGBA8
  // `width` x `height` (straight alpha).
  bool BuildButtonTexture(int index, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
} // namespace ao2::prompts
