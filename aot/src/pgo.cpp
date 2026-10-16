// pgo.cpp - Profile-guided optimization support.
//
// An instrumented build (-fprofile-instr-generate, -DAO2_PGO_GENERATE; see
// tools/pgo.ps1) writes its profile when the file pgo.dump appears in the
// working directory: the game is usually closed by killing it, which skips
// the profile runtime's exit handler.
#if defined(AO2_PGO_GENERATE)
#include <chrono>
#include <filesystem>
#include <thread>

extern "C" int __llvm_profile_write_file(void);

namespace
{
  struct PgoDumper
  {
    PgoDumper()
    {
      std::thread([] {
        for (;;)
        {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          std::error_code ec;
          if (std::filesystem::exists("pgo.dump", ec))
          {
            __llvm_profile_write_file();
            std::filesystem::remove("pgo.dump", ec);
          }
        }
      }).detach();
    }
  } g_pgo_dumper;
} // namespace
#endif
