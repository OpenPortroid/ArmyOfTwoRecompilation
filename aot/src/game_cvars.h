// game_cvars.h - Game-specific CVAR definitions
#pragma once

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(graphics_backend, "any", "GPU",
                      "Graphics API backend: any, d3d12, vulkan")
    .allowed({"any", "d3d12", "vulkan"});


REXCVAR_DEFINE_BOOL(pdz_fps60_unlock, false, "Gameplay",
                    "Unlock 60 FPS");

REXCVAR_DEFINE_BOOL(pdz_aspect_ratio_16_9, false, "Gameplay",
                    "Enable 16:9 aspect ratio");

REXCVAR_DEFINE_BOOL(ao2_fps_unlock, true, "Gameplay",
                    "Unlock FPS. Game speed is tied to FPS, kicks in above ~90FPS");

REXCVAR_DEFINE_INT32(ao2_fps_unlock_mode, 0, "Gameplay",
                     "Frame-rate target used when ao2_fps_unlock is enabled: 0=unlimited (240 cap), 1=60FPS, 2=30FPS");

REXCVAR_DEFINE_INT32(ao2_fps_target, 0, "Gameplay",
                     "Frame-rate cap when ao2_fps_unlock is enabled (e.g. 90, 120, 144); 0 = from ao2_fps_unlock_mode "
                     "(0 = 240, 1 = 60, 2 = 30)");

REXCVAR_DEFINE_INT32(ao2_present_interval, -1, "Gameplay",
                     "VdSwap presentation interval override when ao2_fps_unlock is enabled: -1 = from the mode, "
                     "0 = immediate, 1 = every vblank, 2 = every other vblank");

REXCVAR_DEFINE_BOOL(ao2_disable_msaa, false, "Gameplay",
                    "Black Shading Fix: disables MSAA");

REXCVAR_DEFINE_BOOL(ao2_anisotropic_16x, false, "Gameplay",
                    "16x Anisotropic Filtering");

REXCVAR_DEFINE_BOOL(dev_debug_runtime, false, "Debug",
                    "Enable runtime debug tools (stub sweep, missing function scan)");
