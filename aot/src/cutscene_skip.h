// cutscene_skip.h - Skip cutscenes and movies with a key (see cutscene_skip.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::ui
{
  class Window;
}

namespace ao2::skip
{
  // Starts listening for the skip keys (Enter, Space) in the window (UI thread).
  void Attach(rex::ui::Window *window);

  // Asks for a skip as if the key was pressed (scripted input).
  void Request();

  // Game thread, engine pad polling: performs a pending skip request.
  void Poll(PPCContext &ctx, uint8_t *base);

  // Online co-op: a player's lockstep frame record was replayed (both
  // machines, same frame) - a skip in it is performed here.
  void OnReplayedRecord(PPCContext &ctx, uint8_t *base, uint32_t record, uint32_t controller_id);

  // True if the viewport input `receiver` is the full-screen movie player.
  bool IsMovieReceiver(const uint8_t *base, uint32_t receiver);

  // Pad polling of a movie thread, which no longer goes through the engine
  // (see mouse_aim.cpp): skip requests, and A / Start skip skippable movies.
  void PollMovieThread(PPCContext &ctx, uint8_t *base);

  // From the polling loops of threads that are not the game thread: Poll while a full-screen movie plays, so
  // skip.trigger / Enter / Space work even when the movie's input receiver is not the one PollMovieThread knows.
  void PollWhileMovie(PPCContext &ctx, uint8_t *base);
} // namespace ao2::skip
