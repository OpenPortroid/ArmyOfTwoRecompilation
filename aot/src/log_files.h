// log_files.h - The port's own log files: logs\<area>\<name> under the game folder.
//
// One setting turns every log file off: log_to_files (SDK cvar, category
// "Log", settings/hardware.toml; default false). It also covers the SDK's
// logs (logs\armyoftworecomp_NNN.log, logs\net.log, logs\krnl.log) and the
// launcher's (logs\launcher.log). Read when a log is first opened, so a
// change applies after a restart.
//
// Every log goes into the logs folder of the game, by area, so that a bug
// report is one folder to send and forty files do not lie in the game folder
// next to the assets:
//
//   logs\net\     online play: the co-op link and its quality, the lobby
//                 server, invites, Steam, the lobby a game is in, split-screen
//                 joining (coop_link, coop_net, coop_debug, coop_delay, invite,
//                 ea_server, steam, openportroid, lobby_visibility, split_join,
//                 versus_lobby, versus_guest)
//   logs\sim\     the simulation: the lockstep's commands and LOD, ragdolls,
//                 level streaming, versus bots, their navigation and rules,
//                 the camera and the mouse aim, FOV, cutscenes and movies, the
//                 physics worker (coop_cmd, coop_lod, coop_ragdoll,
//                 coop_streaming, versus_bots, versus_nav, versus_rules,
//                 interp, interp_objects, mouse_aim, fov, cutscene_skip,
//                 control_movie, movie_trace, physx_worker)
//   logs\gpu\     the native renderer (native_gpu)
//   logs\perf\    frame times (decouple, tick_profile, frame_spikes,
//                 actor_profile)
//   logs\ui\      the menus and the port's own windows (menu_mouse,
//                 graphics_menu, ui_blur)
//   logs\audio\   the port's sound settings (audio_settings,
//                 audio_listener_guard)
//   logs\debug\   what a flag or a trigger asks for: traces and dumps
//                 (script_trace, sync_trace, sync_dump.bin, console_cmd,
//                 autopilot, drive_telemetry.csv, state_hash, io_failure)
//   logs\         the runtime's own log (armyoftworecomp_NNN.log), net.log,
//                 krnl.log, the launcher's launcher.log, README.txt (what this
//                 folder is and what to send), and a log not listed above
//
// Debug dumps a trigger file asks for (capture\ with its raw frames,
// frame_trace.txt, gpu_draws.txt, d3d_trace.txt, ui_dump.txt, shader_cache\,
// shader_dump\) are not logs: they stay in the game folder, as do the crash
// and hang dumps (crash_*.txt/.dmp, hang_*.dmp) that hang_watchdog.cpp writes
// next to the exe.
#pragma once

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef AO2_STANDALONE_SERVER
#include <rex/cvar.h>
#endif

namespace ao2
{
  // A log and the folder of logs\ it goes to.
  struct LogAreaName
  {
    const char *name;
    const char *area;
  };

  // Every log the port writes (a name not here goes to logs\ itself).
  inline constexpr LogAreaName kLogAreas[] = {
      // Online play.
      {"coop_link.log", "net"},
      {"coop_net.log", "net"},
      {"coop_debug.log", "net"},
      {"coop_delay.log", "net"},
      {"invite.log", "net"},
      {"ea_server.log", "net"},
      {"steam.log", "net"},
      {"openportroid.log", "net"},
      {"lobby_visibility.log", "net"},
      {"split_join.log", "net"},
      {"versus_lobby.log", "net"},
      {"versus_guest.log", "net"},
      // The simulation.
      {"coop_cmd.log", "sim"},
      {"coop_lod.log", "sim"},
      {"coop_ragdoll.log", "sim"},
      {"coop_streaming.log", "sim"},
      {"versus_bots.log", "sim"},
      {"versus_nav.log", "sim"},
      {"versus_rules.log", "sim"},
      {"interp.log", "sim"},
      {"interp_objects.log", "sim"},
      {"mouse_aim.log", "sim"},
      {"fov.log", "sim"},
      {"cutscene_skip.log", "sim"},
      {"control_movie.log", "sim"},
      {"movie_trace.log", "sim"},
      {"physx_worker.log", "sim"},
      // The native renderer.
      {"native_gpu.log", "gpu"},
      // Frame times.
      {"decouple.log", "perf"},
      {"tick_profile.log", "perf"},
      {"frame_spikes.log", "perf"},
      {"actor_profile.log", "perf"},
      // The menus and the port's windows.
      {"menu_mouse.log", "ui"},
      {"graphics_menu.log", "ui"},
      {"ui_blur.log", "ui"},
      // Sound.
      {"audio_settings.log", "audio"},
      {"audio_listener_guard.log", "audio"},
      // Traces and dumps a flag asked for.
      {"script_trace.log", "debug"},
      {"sync_trace.log", "debug"},
      {"sync_dump.bin", "debug"},
      {"console_cmd.log", "debug"},
      {"autopilot.log", "debug"},
      {"drive_telemetry.csv", "debug"},
      {"state_hash.log", "debug"},
      {"io_failure.log", "debug"},
  };

  // The folder a log belongs to; "" for a name not in the list (logs\ itself).
  inline const char *LogArea(const char *name)
  {
    for (const LogAreaName &entry : kLogAreas)
    {
      if (std::strcmp(entry.name, name) == 0)
      {
        return entry.area;
      }
    }
    return "";
  }

  // logs\<area>\<name>, its folder made. A trace or dump a flag asked for (and
  // that its owner opens itself) goes the same way, so that everything a bug
  // report needs is in one folder.
  inline std::string LogPath(const char *name)
  {
    std::filesystem::path path = "logs";
    if (const char *area = LogArea(name); *area)
    {
      path /= area;
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec); // the fopen fails if it could not
    return (path / name).string();
  }

  // The log file, or nullptr when log files are off (every caller already
  // copes with a log that could not be opened). The standalone lobby server
  // (ao2_lobby_server) has no SDK cvars and always writes its log.
  inline FILE *OpenLogFile(const char *path, const char *mode = "w")
  {
#ifdef AO2_STANDALONE_SERVER
    return std::fopen(LogPath(path).c_str(), mode);
#else
    return REXCVAR_QUERY(bool, log_to_files) ? std::fopen(LogPath(path).c_str(), mode) : nullptr;
#endif
  }

  // logs\README.txt: what the folder is and what to send with a bug report,
  // written once per run (with log files on, else there is nothing to explain).
  // For the player who is asked for logs, not for the code: the launcher's
  // "Open the game folder" leads here.
  inline void WriteLogsReadme()
  {
#ifndef AO2_STANDALONE_SERVER
    if (!REXCVAR_QUERY(bool, log_to_files))
    {
      return;
    }
#endif
    time_t seconds = time(nullptr);
    tm local = {};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    FILE *f = std::fopen(LogPath("README.txt").c_str(), "w");
    if (!f)
    {
      return;
    }
    std::fprintf(f,
                 "Army of Two - the port's logs\n"
                 "=============================\n"
                 "\n"
                 "Everything about a run of the game that a bug report needs. The runtime's\n"
                 "own log is armyoftworecomp_NNN.log (the highest number is the last run), with\n"
                 "net.log and krnl.log next to it; the port's logs are sorted by area:\n"
                 "\n"
                 "  net\\    online play: the co-op link, the lobby server, invites, Steam\n"
                 "  sim\\    the simulation: the lockstep, bots, the camera, cutscenes\n"
                 "  gpu\\    the renderer\n"
                 "  perf\\   frame times\n"
                 "  ui\\     the menus and the port's windows\n"
                 "  audio\\  sound\n"
                 "  debug\\  traces and dumps a flag asked for (sync_trace, state_hash...)\n"
                 "\n"
                 "launcher.log is the launcher's own log, when the launcher sits next to the\n"
                 "game.\n"
                 "\n"
                 "For a bug report: run the game with --log_to_files=true (the launcher's\n"
                 "Settings > Advanced > Extra arguments), play until the problem happens, then\n"
                 "send this whole folder. Crash reports (crash_*.txt and .dmp) stay next to the\n"
                 "exe: send them too. Nothing here is written while log_to_files is off.\n"
                 "\n"
                 "Written %04d-%02d-%02d %02d:%02d:%02d.\n",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    std::fclose(f);
  }
} // namespace ao2
