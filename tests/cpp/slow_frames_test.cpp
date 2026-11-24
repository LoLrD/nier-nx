#include <cstdio>

#include "slow_frames.h"

using nier::native::SlowFrames;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  SlowFrames frames;
  frames.Add({33.3, 20.0, 1.0, 0.5});
  frames.Add({50.0, 35.0, 1.0, 0.5});
  frames.Add({66.7, 10.0, 12.0, 9.0});
  frames.Add({50.0, 12.0, 2.0, 0.0});
  Check(frames.frames == 4 && frames.slow == 3, "frames_over_45_ms_are_slow");
  Check(frames.ring == 1, "slow_frame_with_busy_ring_counted");
  Check(frames.gpu == 1 && frames.uploads == 1, "gpu_wait_and_uploads_counted_together");
  Check(frames.other == 1, "slow_frame_without_known_cause_is_other");
  Check(frames.worst_ms == 66.7, "worst_interval_kept");
  return failures ? 1 : 0;
}
