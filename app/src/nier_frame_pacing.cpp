#include "nier_frame_pacing.h"

#include <algorithm>
#include <atomic>

#include <fmt/format.h>

namespace {

std::array<std::atomic<uint64_t>, 4> intervals{};
std::array<std::atomic<uint64_t>, 3> outcomes{};
std::array<uint64_t, 4> reported_intervals{};
std::array<uint64_t, 3> reported_outcomes{};

template <size_t N>
std::array<uint64_t, N> Delta(std::array<std::atomic<uint64_t>, N>& counts, std::array<uint64_t, N>& reported) {
  std::array<uint64_t, N> delta;
  for (size_t i = 0; i < N; ++i) {
    const uint64_t now = counts[i].load(std::memory_order_relaxed);
    delta[i] = now - reported[i];
    reported[i] = now;
  }
  return delta;
}

}  // namespace

uint32_t NierSoftenSwapParam(uint32_t param, int32_t threshold, int32_t max_interval) {
  if (threshold >= 0) {
    param = (param & ~0xFFu) | uint32_t(std::min(threshold, 255));
  }
  if (max_interval > 0 && ((param >> 8) & 0xF) > uint32_t(max_interval)) {
    param = (param & ~0xF00u) | (uint32_t(max_interval) << 8);
  }
  return param;
}

NierSwapOutcome NierClassifySwap(uint32_t last_target, uint32_t interval, uint32_t vblank_count, bool released_now) {
  if (last_target + interval > vblank_count) {
    return NierSwapOutcome::kOnTime;
  }
  return released_now ? NierSwapOutcome::kLateShownNow : NierSwapOutcome::kLateNextVblank;
}

void NierCountSwap(uint32_t requested_interval, NierSwapOutcome outcome) {
  intervals[std::min<uint32_t>(requested_interval, 3)].fetch_add(1, std::memory_order_relaxed);
  outcomes[size_t(outcome)].fetch_add(1, std::memory_order_relaxed);
}

std::string NierFramePacingReport() {
  const auto asked = Delta(intervals, reported_intervals);
  const auto shown = Delta(outcomes, reported_outcomes);
  return fmt::format("frame pacing: asked interval 1x {}, 2x {}, 3x {}; on time {}, late shown now {}, late held to "
                     "next vblank {}",
                     asked[1], asked[2], asked[3], shown[0], shown[1], shown[2]);
}
