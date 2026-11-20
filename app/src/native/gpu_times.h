#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace nier::native {

enum class GpuCategory : uint8_t { kUploads, kDraws, kResolves, kPresent, kTransfers, kNone };

constexpr uint32_t kGpuCategories = 5;

void AddGpuTimes(const uint64_t* ticks, const GpuCategory* categories, uint32_t count, double period_ns,
                 uint64_t valid_mask, std::array<uint64_t, kGpuCategories>& ns);
void AddPassTimes(const uint64_t* ticks, const uint16_t* passes, uint32_t count, double period_ns,
                  uint64_t valid_mask, std::vector<uint64_t>& ns);

}  // namespace nier::native
