#include "read_patches.h"

#include <algorithm>
#include <cstring>

namespace nier {

void ApplyReadPatches(std::span<const ReadPatch> patches, uint64_t offset, std::span<uint8_t> data) {
  const uint64_t end = offset + data.size();
  for (const ReadPatch& patch : patches) {
    const uint64_t from = std::max(offset, patch.begin);
    const uint64_t to = std::min(end, patch.begin + patch.bytes.size());
    if (from < to) {
      std::memcpy(data.data() + (from - offset), patch.bytes.data() + (from - patch.begin), size_t(to - from));
    }
  }
}

std::optional<ReadPatch> TextPatch(std::span<const uint8_t> file, uint64_t base, std::string_view original,
                                   std::string_view replacement) {
  if (original.size() != replacement.size()) {
    return std::nullopt;
  }
  const std::string_view text(reinterpret_cast<const char*>(file.data()), file.size());
  const size_t at = text.find(original);
  if (at == std::string_view::npos || text.find(original, at + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  return ReadPatch{base + at, {replacement.begin(), replacement.end()}};
}

uint64_t Fnv1a64(std::span<const uint8_t> data) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const uint8_t byte : data) {
    hash = (hash ^ byte) * 0x100000001b3ull;
  }
  return hash;
}

}  // namespace nier
