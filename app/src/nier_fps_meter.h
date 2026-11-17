#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

struct NierFpsSample {
  double fps = 0;
  double frame_time_ms = 0;
  uint64_t frames = 0;
};

class NierFpsMeter {
 public:
  NierFpsSample Update(uint64_t swaps, std::chrono::steady_clock::time_point now) {
    if (!window_start_) {
      window_start_ = now;
      window_swaps_ = swaps;
      first_swaps_ = swaps;
      return sample_;
    }
    const auto elapsed = std::chrono::duration<double>(now - *window_start_).count();
    if (elapsed < kWindowSeconds) {
      return sample_;
    }
    sample_.fps = double(swaps - window_swaps_) / elapsed;
    sample_.frame_time_ms = sample_.fps > 0 ? 1000.0 / sample_.fps : 0;
    sample_.frames = swaps - first_swaps_;
    window_start_ = now;
    window_swaps_ = swaps;
    return sample_;
  }

 private:
  static constexpr double kWindowSeconds = 0.5;
  std::optional<std::chrono::steady_clock::time_point> window_start_;
  uint64_t window_swaps_ = 0;
  uint64_t first_swaps_ = 0;
  NierFpsSample sample_;
};
