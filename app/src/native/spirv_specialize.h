#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace nier::native {

struct ByteRange {
  uint32_t begin;
  uint32_t end;
};

struct UniformSlot {
  bool fetch;
  uint32_t offset;

  auto operator<=>(const UniformSlot&) const = default;
};

inline constexpr uint32_t kFetchConstantBytes = 24;

std::vector<UniformSlot> SpecializableSlots(std::span<const uint32_t> module, std::span<const ByteRange> system_ranges);
std::vector<UniformSlot> TextureFetchSlots(std::vector<UniformSlot> slots,
                                           std::span<const uint32_t> texture_fetch_constants);
std::vector<uint32_t> Specialize(std::span<const uint32_t> module, std::span<const ByteRange> system_ranges,
                                 std::span<const UniformSlot> slots, std::span<const uint32_t> values);
std::vector<uint32_t> AllowContraction(std::span<const uint32_t> module);
std::vector<uint32_t> ForceEarlyFragmentTests(std::span<const uint32_t> module);

}  // namespace nier::native
