#include "slow_frames.h"

#include <algorithm>

namespace nier::native {

void SlowFrames::Add(const FrameCost& frame) {
  ++frames;
  worst_ms = std::max(worst_ms, frame.interval_ms);
  if (frame.interval_ms <= kSlowMs) {
    return;
  }
  ++slow;
  const bool by_ring = frame.ring_ms > kRingMs;
  const bool by_gpu = frame.gpu_wait_ms > kGpuWaitMs;
  const bool by_uploads = frame.upload_mb > kUploadMb;
  ring += by_ring;
  gpu += by_gpu;
  uploads += by_uploads;
  other += !by_ring && !by_gpu && !by_uploads;
}

}  // namespace nier::native
