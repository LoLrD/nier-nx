#include "nier_npc_lod.h"

#include <atomic>

#include <fmt/format.h>

namespace {

constexpr size_t kBins = kNierNpcSizeBins.size() + 2;
std::array<std::atomic<uint64_t>, kBins> counts{};
std::array<uint64_t, kBins> reported{};

}  // namespace

size_t NierNpcSizeBin(double size) {
  if (size >= kNierNpcSizeSentinel) {
    return kBins - 1;
  }
  for (size_t i = 0; i < kNierNpcSizeBins.size(); ++i) {
    if (size <= kNierNpcSizeBins[i]) {
      return i;
    }
  }
  return kNierNpcSizeBins.size();
}

double NierNpcScaledSize(double size, double scale) { return size >= kNierNpcSizeSentinel ? size : size * scale; }

void NierNpcCount(double size) { counts[NierNpcSizeBin(size)].fetch_add(1, std::memory_order_relaxed); }

std::string NierNpcLodReport(double scale) {
  std::array<uint64_t, kBins> delta;
  uint64_t total = 0;
  for (size_t i = 0; i < kBins; ++i) {
    const uint64_t now = counts[i].load(std::memory_order_relaxed);
    delta[i] = now - reported[i];
    reported[i] = now;
    total += delta[i];
  }
  return fmt::format("npc lod: scale {:.2f}, {} size checks, <=0.03 {}, <=0.04 {}, <=0.05 {}, <=0.06 {}, larger {}, "
                     "always {}",
                     scale, total, delta[0], delta[1], delta[2], delta[3], delta[4], delta[5]);
}
