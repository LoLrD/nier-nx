#include <cstdio>

#include "upscale_plan.h"

using nier::native::PlanSceneUpscale;
using nier::native::SceneUpscalePlan;

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
  return failures ? 1 : 0;
}
