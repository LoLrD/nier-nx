#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nier {

struct ReadPatch {
  uint64_t begin;
  std::vector<uint8_t> bytes;
};

void ApplyReadPatches(std::span<const ReadPatch> patches, uint64_t offset, std::span<uint8_t> data);
std::optional<ReadPatch> TextPatch(std::span<const uint8_t> file, uint64_t base, std::string_view original,
                                   std::string_view replacement);
uint64_t Fnv1a64(std::span<const uint8_t> data);

}  // namespace nier
