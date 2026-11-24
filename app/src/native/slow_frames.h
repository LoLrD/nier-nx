#pragma once

#include <cstdint>

namespace nier::native {

struct FrameCost {
  double interval_ms;
  double ring_ms;
  double gpu_wait_ms;
  double upload_mb;
};

class SlowFrames {
 public:
  static constexpr double kSlowMs = 45.0;
  static constexpr double kRingMs = 30.0;
  static constexpr double kGpuWaitMs = 8.0;
  static constexpr double kUploadMb = 4.0;

  void Add(const FrameCost& frame);

  uint64_t frames = 0;
  uint64_t slow = 0;
  uint64_t ring = 0;
  uint64_t gpu = 0;
  uint64_t uploads = 0;
  uint64_t other = 0;
  double worst_ms = 0.0;
};

}  // namespace nier::native
