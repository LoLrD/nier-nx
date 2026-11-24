#include <cstdio>
#include <vector>

#include "upscale_plan.h"

using nier::native::GpuGovernor;
using nier::native::PlanSceneUpscale;
using nier::native::Quality;
using nier::native::QualityLadder;
using nier::native::QualityOptions;
using nier::native::SceneUpscalePlan;

static int failures = 0;
static constexpr double kFrameMs = 33.3;

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
    const QualityOptions all{true, true, true, true};
    const std::vector<Quality> ladder = QualityLadder(all);
    Check(ladder.size() == 5, "every_adaptive_option_is_a_step");
    Check(ladder.back() == Quality{true, true, true, true}, "best_step_has_everything");
    Check(ladder[3] == Quality{false, true, true, true}, "sharpening_goes_first");
    Check(ladder[2] == Quality{false, true, false, true}, "then_the_scene_resolution");
    Check(ladder[1] == Quality{false, true, false, false}, "then_the_shadow_resolution");
    Check(ladder.front() == Quality{}, "the_upscale_goes_last");
    Check(QualityLadder({true, true, false, true}).size() == 4, "fixed_scene_has_no_resolution_step");
    for (const Quality& quality : QualityLadder({true, true, false, true})) {
      Check(quality.full_scene, "fixed_scene_stays_full");
    }
    for (const Quality& quality : QualityLadder({false, true, true, true})) {
      Check(!quality.upscale && !quality.sharpen, "no_fsr_means_no_upscale_or_sharpening");
    }
    Check(QualityLadder({false, true, true, true}).size() == 3, "without_fsr_scene_and_shadows_adapt");
    Check(QualityLadder({true, false, true, true}).size() == 4, "without_sharpening_there_is_no_sharpening_step");
    const std::vector<Quality> fixed = QualityLadder({false, false, false, false});
    Check(fixed.size() == 1 && fixed[0].full_scene && fixed[0].full_shadows, "nothing_adaptive_is_one_full_step");
  }
  {
    GpuGovernor governor({true, true, true, true}, 30.0);
    Check(governor.quality() == Quality{true, true, true, true}, "starts_at_the_best_step");
    governor.Window(31.0, kFrameMs);
    Check(governor.quality() == Quality{false, true, true, true}, "overloaded_gpu_drops_sharpening");
    governor.Window(32.0, kFrameMs);
    Check(governor.quality() == Quality{false, true, false, true}, "still_overloaded_lowers_the_scene_resolution");
    governor.Window(31.0, kFrameMs);
    Check(governor.quality() == Quality{false, true, false, false}, "then_lowers_the_shadow_resolution");
    governor.Window(31.0, kFrameMs);
    Check(governor.quality() == Quality{}, "then_drops_the_upscale");
    governor.Window(31.0, kFrameMs);
    Check(governor.level() == 0, "never_below_the_lowest_step");
    for (int i = 0; i < 3; ++i) {
      governor.Window(25.5, kFrameMs);
    }
    Check(governor.level() == 0, "recovery_waits_for_calm_windows");
    governor.Window(25.5, kFrameMs);
    Check(governor.quality() == Quality{false, true, false, false}, "calm_gpu_brings_the_upscale_back");
    for (int i = 0; i < 4; ++i) {
      governor.Window(27.5, kFrameMs);
    }
    Check(governor.quality() == Quality{false, true, false, true}, "then_the_shadow_resolution");
    for (int i = 0; i < 4; ++i) {
      governor.Window(25.5, kFrameMs);
    }
    Check(governor.quality() == Quality{false, true, false, true}, "scene_resolution_needs_more_headroom");
    for (int i = 0; i < 4; ++i) {
      governor.Window(24.5, kFrameMs);
    }
    Check(governor.quality() == Quality{false, true, true, true}, "enough_headroom_brings_the_scene_resolution_back");
    for (int i = 0; i < 4; ++i) {
      governor.Window(27.5, kFrameMs);
    }
    Check(governor.quality() == Quality{false, true, true, true}, "sharpening_needs_its_headroom");
    for (int i = 0; i < 4; ++i) {
      governor.Window(27.0, kFrameMs);
    }
    Check(governor.quality() == Quality{true, true, true, true}, "headroom_brings_sharpening_back");
    governor.Window(29.0, kFrameMs);
    Check(governor.level() == 4, "busy_window_inside_budget_keeps_the_step");
  }
  {
    GpuGovernor governor({true, true, true, true}, 30.0);
    governor.Window(31.0, kFrameMs);
    governor.Configure({true, true, true, true}, 30.0);
    Check(governor.level() == 3, "same_options_keep_the_step");
    governor.Configure({true, false, true, true}, 30.0);
    Check(governor.quality() == Quality{false, true, true, true}, "changed_options_restart_at_the_best_step");
    governor.Configure({false, false, false, false}, 30.0);
    governor.Window(40.0, kFrameMs);
    Check(governor.quality() == Quality{false, false, true, true}, "nothing_adaptive_never_steps_down");
  }
  {
    GpuGovernor governor({true, true, true, true}, 30.0);
    governor.Window(31.0, 41.6);
    Check(governor.level() == 4, "busy_gpu_in_a_cpu_bound_frame_keeps_the_quality");
    governor.Window(31.0, 33.3);
    Check(governor.level() == 3, "gpu_bound_frame_over_budget_steps_down");
    governor.Window(38.0, 41.6);
    Check(governor.level() == 2, "slow_gpu_bound_frame_steps_down");
  }
  return failures ? 1 : 0;
}
