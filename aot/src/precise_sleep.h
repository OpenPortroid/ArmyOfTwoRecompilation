// precise_sleep.h - Sleeping until a moment on a high-resolution timer, waking
// early enough for the timer's real lateness.
//
// A high-resolution waitable timer wakes ~0.5 ms late while the game's window
// is in the foreground, but several milliseconds late when it is not (two
// co-op instances on one PC: the one without focus fell from 120 to 70-90 FPS
// with a fixed 0.6 ms margin). The margin follows the lateness seen: the
// largest recent one (decaying by 1% a wake) plus a little, never below the
// caller's minimum - and, while another process has the foreground window,
// never below 3 ms (measured lateness alone still let the frame rate drop;
// a fixed 3 ms held 120 FPS). What is left after the wake the caller spins.
#pragma once

#include <windows.h>

#include <algorithm>

namespace ao2
{
  class PreciseSleeper
  {
  public:
    // Sleeps `seconds` minus the margin (nothing if that is not positive).
    void Sleep(double seconds, double min_margin_s)
    {
      if (!InForeground())
      {
        min_margin_s = std::max(min_margin_s, kBackgroundMarginS);
      }
      double margin = std::clamp(late_s_ + kExtraS, min_margin_s, kMaxMarginS);
      double request = seconds - margin;
      if (request <= 0.0)
      {
        return;
      }
      if (!timer_)
      {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      }
      LARGE_INTEGER due;
      due.QuadPart = -LONGLONG(request * 1e7);
      if (!timer_ || due.QuadPart >= 0 || !SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
      {
        return;
      }
      LARGE_INTEGER freq, start, end;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&start);
      WaitForSingleObject(timer_, 100);
      QueryPerformanceCounter(&end);
      double late = double(end.QuadPart - start.QuadPart) / double(freq.QuadPart) - request;
      late_s_ = std::max(std::max(late, 0.0), late_s_ * 0.99);
    }

    // The margin the next sleep leaves (for statistics).
    double LatenessS() const
    {
      return late_s_;
    }

  private:
    // Whether the foreground window is this process's (looked up every 100 ms).
    static bool InForeground()
    {
      static thread_local ULONGLONG checked = 0;
      static thread_local bool foreground = true;
      ULONGLONG now = GetTickCount64();
      if (now - checked >= 100)
      {
        checked = now;
        DWORD pid = 0;
        HWND window = GetForegroundWindow();
        foreground = window && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId();
      }
      return foreground;
    }

    static constexpr double kExtraS = 0.0002;
    static constexpr double kBackgroundMarginS = 0.003;
    static constexpr double kMaxMarginS = 0.006;
    HANDLE timer_ = nullptr;
    double late_s_ = 0.0;
  };
} // namespace ao2
