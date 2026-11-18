#pragma once

#include <array>
#include <cstdint>

namespace nier::native {

enum class GpuCategory : uint8_t { kUploads, kDraws, kResolves, kPresent, kNone };

constexpr uint32_t kGpuCategories = 4;

void AddGpuTimes(const uint64_t* ticks, const GpuCategory* categories, uint32_t count, double period_ns,
                 uint64_t valid_mask, std::array<uint64_t, kGpuCategories>& ns);

}  // namespace nier::native
