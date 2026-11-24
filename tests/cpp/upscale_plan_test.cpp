#include <cstdio>

#include "upscale_plan.h"

using nier::native::PlanSceneUpscale;
using nier::native::SceneUpscalePlan;
using nier::native::UpscaleGovernor;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  const SceneUpscalePlan half = PlanSceneUpscale(640, 360, 1280);
  Check(half.wanted && half.width == 1280 && half.height == 720, "half_resolution_scene_goes_to_720p");
  const SceneUpscalePlan level = PlanSceneUpscale(768, 432, 1280);
  Check(level.wanted && level.width == 1280 && level.height == 720, "sixty_percent_scene_goes_to_720p");
  Check(!PlanSceneUpscale(1280, 720, 1280).wanted, "full_size_image_is_left_alone");
  Check(!PlanSceneUpscale(512, 512, 1280).wanted, "square_bloom_image_is_left_alone");
  Check(!PlanSceneUpscale(256, 144, 1280).wanted, "tiny_blur_image_is_left_alone");
  Check(!PlanSceneUpscale(640, 360, 640).wanted, "scene_sized_pass_is_left_alone");
  {
    UpscaleGovernor governor(2, 30.0);
    Check(governor.level() == 2, "starts_at_the_best_level");
    governor.Window(31.0);
    Check(governor.level() == 1, "overloaded_gpu_drops_sharpening");
    governor.Window(32.0);
    Check(governor.level() == 0, "still_overloaded_drops_the_upscale");
    governor.Window(31.0);
    Check(governor.level() == 0, "never_below_off");
    for (int i = 0; i < 3; ++i) {
      governor.Window(24.0);
    }
    Check(governor.level() == 0, "recovery_waits_for_calm_windows");
    governor.Window(24.0);
    Check(governor.level() == 1, "calm_gpu_brings_the_upscale_back");
    for (int i = 0; i < 4; ++i) {
      governor.Window(27.5);
    }
    Check(governor.level() == 1, "sharpening_needs_more_headroom");
    for (int i = 0; i < 4; ++i) {
      governor.Window(26.0);
    }
    Check(governor.level() == 2, "enough_headroom_brings_sharpening_back");
    governor.Window(29.0);
    governor.Window(26.0);
    governor.Window(26.0);
    governor.Window(26.0);
    Check(governor.level() == 2, "busy_window_inside_budget_keeps_the_level");
  }
  {
    UpscaleGovernor governor(1, 30.0);
    for (int i = 0; i < 8; ++i) {
      governor.Window(10.0);
    }
    Check(governor.level() == 1, "never_above_the_allowed_level");
  }
  return failures ? 1 : 0;
}
