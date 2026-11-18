#include <chrono>
#include <cstdint>

#include <rex/hook.h>

#include "native/gpu_progress.h"

REX_EXTERN(__imp__sub_82579D50);

namespace {

constexpr std::chrono::microseconds kPollTimeout{2000};
thread_local uint64_t seen_progress = 0;

}  // namespace

REX_HOOK_RAW(sub_82579D50) {
  if (nier::native::GpuProgress* progress = nier::native::ActiveGpuProgress()) {
    seen_progress = progress->WaitPast(seen_progress, kPollTimeout);
  }
  __imp__sub_82579D50(ctx, base);
}
