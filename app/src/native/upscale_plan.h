#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

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

struct Quality {
  bool sharpen = false;
  bool upscale = false;
  bool full_scene = false;
  bool full_shadows = false;

  bool operator==(const Quality&) const = default;
};

struct QualityOptions {
  bool upscale = false;
  bool sharpen = false;
  bool dynamic_scene = false;
  bool adaptive_shadows = false;

  bool operator==(const QualityOptions&) const = default;
};

inline std::vector<Quality> QualityLadder(const QualityOptions& options) {
  Quality quality{options.upscale && options.sharpen, options.upscale, true, true};
  std::vector<Quality> ladder{quality};
  const auto step_down = [&](bool Quality::*feature, bool adaptive) {
    if (adaptive && quality.*feature) {
      quality.*feature = false;
      ladder.push_back(quality);
    }
  };
  step_down(&Quality::sharpen, true);
  step_down(&Quality::full_scene, options.dynamic_scene);
  step_down(&Quality::full_shadows, options.adaptive_shadows);
  step_down(&Quality::upscale, true);
  std::reverse(ladder.begin(), ladder.end());
  return ladder;
}

inline std::atomic<bool> g_scene_reduced{false};

class GpuGovernor {
 public:
  GpuGovernor(const QualityOptions& options, double budget_ms)
      : options_(options), ladder_(QualityLadder(options)), level_(int(ladder_.size()) - 1), budget_ms_(budget_ms) {}

  int level() const { return level_; }
  const Quality& quality() const { return ladder_[level_]; }

  void Configure(const QualityOptions& options, double budget_ms) {
    budget_ms_ = budget_ms;
    if (options == options_) {
      return;
    }
    options_ = options;
    ladder_ = QualityLadder(options);
    level_ = int(ladder_.size()) - 1;
    calm_ = 0;
  }

  void Window(double gpu_ms) {
    if (gpu_ms > budget_ms_) {
      level_ = std::max(0, level_ - 1);
      calm_ = 0;
      return;
    }
    if (level_ + 1 < int(ladder_.size()) &&
        gpu_ms + StepUpMs(ladder_[level_], ladder_[level_ + 1]) + kMarginMs <= budget_ms_) {
      if (++calm_ >= kCalmWindows) {
        ++level_;
        calm_ = 0;
      }
    } else {
      calm_ = 0;
    }
  }

 private:
  static double StepUpMs(const Quality& from, const Quality& to) {
    return (to.sharpen && !from.sharpen ? kSharpenMs : 0.0) + (to.upscale && !from.upscale ? kUpscaleMs : 0.0) +
           (to.full_scene && !from.full_scene ? kFullSceneMs : 0.0) +
           (to.full_shadows && !from.full_shadows ? kFullShadowsMs : 0.0);
  }

  static constexpr double kSharpenMs = 2.0;
  static constexpr double kUpscaleMs = 3.5;
  static constexpr double kFullSceneMs = 4.0;
  static constexpr double kFullShadowsMs = 1.5;
  static constexpr double kMarginMs = 1.0;
  static constexpr int kCalmWindows = 4;

  QualityOptions options_;
  std::vector<Quality> ladder_;
  int level_;
  double budget_ms_;
  int calm_ = 0;
};

}  // namespace nier::native
