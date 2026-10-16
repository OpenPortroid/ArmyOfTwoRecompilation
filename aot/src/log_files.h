// log_files.h - The port's own log files (*.log in the working directory).
//
// One setting turns every log file off: log_to_files (SDK cvar, category
// "Log", settings/hardware.toml; default false). It also covers the SDK's
// logs (logs/armyoftworecomp_NNN.log, net.log, krnl.log). Read when a log is
// first opened, so a change applies after a restart. Debug dumps a flag or
// trigger file asks for (capture/, frame_trace.txt, sync_dump.bin, ...) are
// not logs and stay as they are.
#pragma once

#include <cstdio>

#ifndef AO2_STANDALONE_SERVER
#include <rex/cvar.h>
#endif

namespace ao2
{
  // The log file, or nullptr when log files are off (every caller already
  // copes with a log that could not be opened). The standalone lobby server
  // (ao2_lobby_server) has no SDK cvars and always writes its log.
  inline FILE *OpenLogFile(const char *path, const char *mode = "w")
  {
#ifdef AO2_STANDALONE_SERVER
    return std::fopen(path, mode);
#else
    return REXCVAR_QUERY(bool, log_to_files) ? std::fopen(path, mode) : nullptr;
#endif
  }
} // namespace ao2
