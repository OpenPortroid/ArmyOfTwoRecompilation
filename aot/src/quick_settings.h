// quick_settings.h - The F3 quick settings window (see quick_settings.cpp).
#pragma once

#include <string>

namespace ao2::quick
{
  // The key that opens it (ao2_quick_settings_key; empty = none).
  std::string Key();

  // UI thread: opens the window, or closes it when it is open.
  void Toggle();
} // namespace ao2::quick
