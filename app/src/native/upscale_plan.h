#pragma once

#include <cstdint>

namespace nier::native {

struct SceneUpscalePlan {
  bool wanted = false;
  uint32_t width = 0;
  uint32_t height = 0;
};

inline SceneUpscalePlan PlanSceneUpscale(uint32_t source_width, uint32_t source_height, uint32_t target_width) {
  if (source_width >= target_width || source_width * 4 < target_width || source_width * 9 != source_height * 16) {
    return {};
  }
  return {true, target_width, source_height * target_width / source_width};
}

}  // namespace nier::native
