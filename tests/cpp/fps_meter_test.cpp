#include <chrono>
#include <cstdio>

#include "nier_fps_meter.h"

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  NierFpsMeter meter;
  const auto t0 = Clock::time_point{} + milliseconds(1000);
  auto first = meter.Update(100, t0);
  Check(first.frames == 0, "no rate before the first window ends");

  auto mid = meter.Update(110, t0 + milliseconds(250));
  Check(mid.frames == 0, "still no rate inside the first window");

  auto full = meter.Update(115, t0 + milliseconds(500));
  Check(full.frames == 15, "frames counted since the first sample");
  Check(full.fps > 29.9 && full.fps < 30.1, "15 swaps in 500 ms is 30 fps");
  Check(full.frame_time_ms > 33.2 && full.frame_time_ms < 33.4, "30 fps is 33.3 ms per frame");

  auto held = meter.Update(118, t0 + milliseconds(700));
  Check(held.fps > 29.9 && held.fps < 30.1, "rate held until the next window ends");

  auto next = meter.Update(145, t0 + milliseconds(1000));
  Check(next.fps > 59.9 && next.fps < 60.1, "30 swaps in the next 500 ms is 60 fps");

  return failures == 0 ? 0 : 1;
}
