#pragma once

#include <algorithm>
#include <cstdint>

namespace nier::native {

struct SceneUpscalePlan {
  bool wanted = false;
  uint32_t width = 0;
  uint32_t height = 0;
};

inline SceneUpscalePlan PlanSceneUpscale(uint32_t source_width, uint32_t source_height, uint32_t target_width) {
  if (source_width * 5 > target_width * 4 || source_width * 4 < target_width || source_width * 9 != source_height * 16) {
    return {};
  }
  return {true, target_width, source_height * target_width / source_width};
}

class GpuGovernor {
 public:
  GpuGovernor(int max_level, int min_level, double budget_ms)
      : max_level_(max_level), min_level_(min_level), level_(max_level), budget_ms_(budget_ms) {}

  int level() const { return level_; }

  void Limit(int max_level, int min_level, double budget_ms) {
    max_level_ = max_level;
    min_level_ = std::min(min_level, max_level);
    level_ = std::clamp(level_, min_level_, max_level_);
    budget_ms_ = budget_ms;
  }

  void Window(double gpu_ms) {
    if (gpu_ms > budget_ms_) {
      level_ = std::max(min_level_, level_ - 1);
      calm_ = 0;
      return;
    }
    if (level_ < max_level_ && gpu_ms + kStepUpMs[level_] + kMarginMs <= budget_ms_) {
      if (++calm_ >= kCalmWindows) {
        ++level_;
        calm_ = 0;
      }
    } else {
      calm_ = 0;
    }
  }

 private:
  static constexpr double kStepUpMs[3] = {1.5, 3.5, 2.0};
  static constexpr double kMarginMs = 1.0;
  static constexpr int kCalmWindows = 4;

  int max_level_;
  int min_level_;
  int level_;
  double budget_ms_;
  int calm_ = 0;
};

}  // namespace nier::native
