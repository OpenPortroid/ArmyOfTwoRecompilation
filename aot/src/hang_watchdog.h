// hang_watchdog.h - Minidump when the game stops presenting frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ao2::hang
{
  // Called on every vblank heartbeat with the swap count. True when a hang
  // dump was just written (its file name in `path`).
  bool OnHeartbeat(uint64_t swaps, char *path, size_t path_size);

  // Once at startup: a crash writes crash_<date>_<time>.dmp / .txt.
  void InstallCrashHandler();

  // One line per guest thread: name, LR and the return addresses found by
  // walking its guest stack (for the log: the minidump has no guest memory).
  // `registers`: also its context's GPRs and the top of its stack (crashes).
  std::string GuestStacks(bool registers = false);
} // namespace ao2::hang
