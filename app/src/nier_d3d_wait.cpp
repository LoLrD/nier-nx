#include <chrono>
#include <cstdint>
#include <limits>

#include <rex/hook.h>

#include "native/gpu_progress.h"

REX_EXTERN(__imp__sub_82579C58);
REX_EXTERN(__imp__sub_82579D50);

namespace {

constexpr std::chrono::microseconds kPollTimeout{2000};
constexpr uint64_t kFreshWait = std::numeric_limits<uint64_t>::max();
thread_local uint64_t seen_progress = kFreshWait;

}  // namespace

REX_HOOK_RAW(sub_82579C58) {
  seen_progress = kFreshWait;
  __imp__sub_82579C58(ctx, base);
}

REX_HOOK_RAW(sub_82579D50) {
  if (nier::native::GpuProgress* progress = nier::native::ActiveGpuProgress()) {
    seen_progress = progress->WaitPast(seen_progress, kPollTimeout);
  }
  __imp__sub_82579D50(ctx, base);
}
