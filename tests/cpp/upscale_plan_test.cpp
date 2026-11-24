#include <cstdio>

#include "upscale_plan.h"

using nier::native::PlanSceneUpscale;
using nier::native::SceneUpscalePlan;
using nier::native::GpuGovernor;

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
  const SceneUpscalePlan low = PlanSceneUpscale(512, 288, 1280);
  Check(low.wanted && low.width == 1280 && low.height == 720, "forty_percent_scene_goes_to_720p");
  Check(PlanSceneUpscale(1024, 576, 1280).wanted, "eighty_percent_scene_goes_to_720p");
  Check(!PlanSceneUpscale(768, 432, 800).wanted, "pass_padded_to_edram_tiles_is_left_alone");
  Check(!PlanSceneUpscale(1024, 576, 1040).wanted, "pass_padded_to_edram_tiles_at_1024_is_left_alone");
  Check(!PlanSceneUpscale(512, 288, 560).wanted, "pass_padded_to_edram_tiles_at_512_is_left_alone");
  {
    GpuGovernor governor(3, 0, 30.0);
    Check(governor.level() == 3, "starts_at_the_best_level");
    governor.Window(31.0);
    Check(governor.level() == 2, "overloaded_gpu_drops_sharpening");
    governor.Window(32.0);
    Check(governor.level() == 1, "still_overloaded_drops_the_upscale");
    governor.Window(31.0);
    Check(governor.level() == 0, "then_drops_shadow_resolution");
    governor.Window(31.0);
    Check(governor.level() == 0, "never_below_the_lowest_level");
    for (int i = 0; i < 3; ++i) {
      governor.Window(27.0);
    }
    Check(governor.level() == 0, "recovery_waits_for_calm_windows");
    governor.Window(27.0);
    Check(governor.level() == 1, "calm_gpu_brings_shadows_back");
    for (int i = 0; i < 4; ++i) {
      governor.Window(26.0);
    }
    Check(governor.level() == 1, "upscale_needs_more_headroom");
    for (int i = 0; i < 4; ++i) {
      governor.Window(25.0);
    }
    Check(governor.level() == 2, "enough_headroom_brings_the_upscale_back");
    for (int i = 0; i < 4; ++i) {
      governor.Window(27.5);
    }
    Check(governor.level() == 2, "sharpening_needs_its_headroom");
    for (int i = 0; i < 4; ++i) {
      governor.Window(26.0);
    }
    Check(governor.level() == 3, "headroom_brings_sharpening_back");
    governor.Window(29.0);
    Check(governor.level() == 3, "busy_window_inside_budget_keeps_the_level");
  }
  {
    GpuGovernor governor(1, 1, 30.0);
    governor.Window(40.0);
    Check(governor.level() == 1, "never_below_the_allowed_lowest_level");
    for (int i = 0; i < 8; ++i) {
      governor.Window(10.0);
    }
    Check(governor.level() == 1, "never_above_the_allowed_level");
  }
  return failures ? 1 : 0;
}
