#include "gpu_times.h"

#include <cstddef>

namespace nier::native {

void AddGpuTimes(const uint64_t* ticks, const GpuCategory* categories, uint32_t count, double period_ns,
                 uint64_t valid_mask, std::array<uint64_t, kGpuCategories>& ns) {
  for (uint32_t i = 0; i + 1 < count; ++i) {
    if (categories[i] == GpuCategory::kNone) {
      continue;
    }
    const uint64_t delta = (ticks[i + 1] - ticks[i]) & valid_mask;
    ns[size_t(categories[i])] += uint64_t(double(delta) * period_ns);
  }
}

void AddPassTimes(const uint64_t* ticks, const uint16_t* passes, uint32_t count, double period_ns,
                  uint64_t valid_mask, std::vector<uint64_t>& ns) {
  for (uint32_t i = 0; i + 1 < count; ++i) {
    if (!passes[i]) {
      continue;
    }
    if (ns.size() <= passes[i]) {
      ns.resize(passes[i] + 1);
    }
    const uint64_t delta = (ticks[i + 1] - ticks[i]) & valid_mask;
    ns[passes[i]] += uint64_t(double(delta) * period_ns);
  }
}

}  // namespace nier::native
