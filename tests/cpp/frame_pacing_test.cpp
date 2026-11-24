#include <cstdio>
#include <string>

#include "nier_frame_pacing.h"

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  Check(NierSoftenSwapParam(0x00000300, 100, 2) == 0x00000264, "late_threshold_set_and_interval_three_clamped");
  Check(NierSoftenSwapParam(0x00000201, 100, 2) == 0x00000264, "interval_two_kept");
  Check(NierSoftenSwapParam(0x00000100, 100, 2) == 0x00000164, "menu_interval_one_kept");
  Check(NierSoftenSwapParam(0x00000300, -1, 0) == 0x00000300, "negative_and_zero_leave_the_game_values");
  Check(NierSoftenSwapParam(0x00010300, 100, 0) == 0x00010364, "frontbuffer_bits_kept");
  Check(NierClassifySwap(10, 2, 11, false) == NierSwapOutcome::kOnTime, "target_after_count_is_on_time");
  Check(NierClassifySwap(10, 2, 12, true) == NierSwapOutcome::kLateShownNow, "late_frame_released_now");
  Check(NierClassifySwap(10, 2, 12, false) == NierSwapOutcome::kLateNextVblank, "late_frame_held_to_next_vblank");
  NierCountSwap(2, NierSwapOutcome::kOnTime);
  NierCountSwap(3, NierSwapOutcome::kLateShownNow);
  NierCountSwap(2, NierSwapOutcome::kLateNextVblank);
  Check(NierFramePacingReport() ==
            "frame pacing: asked interval 1x 0, 2x 2, 3x 1; on time 1, late shown now 1, late held to next vblank 1",
        "report_counts_since_last");
  Check(NierFramePacingReport().find("on time 0,") != std::string::npos, "report_resets");
  return failures ? 1 : 0;
}
