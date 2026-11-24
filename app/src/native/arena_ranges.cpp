#include "arena_ranges.h"

#include <iterator>

namespace nier::native {

uint32_t ArenaRanges::AddBlock() {
  blocks_.push_back({{0, block_bytes_}});
  free_bytes_ += block_bytes_;
  return uint32_t(blocks_.size() - 1);
}

std::optional<ArenaRange> ArenaRanges::Allocate(uint64_t size, uint64_t alignment) {
  for (uint32_t block = 0; block < blocks_.size(); ++block) {
    auto& free = blocks_[block];
    for (auto it = free.begin(); it != free.end(); ++it) {
      const auto [start, length] = *it;
      const uint64_t aligned = (start + alignment - 1) / alignment * alignment;
      if (aligned + size > start + length) {
        continue;
      }
      free.erase(it);
      if (aligned > start) {
        free.emplace(start, aligned - start);
      }
      if (aligned + size < start + length) {
        free.emplace(aligned + size, start + length - aligned - size);
      }
      free_bytes_ -= size;
      return ArenaRange{block, aligned, size};
    }
  }
  return std::nullopt;
}

void ArenaRanges::Free(const ArenaRange& range) {
  auto& free = blocks_[range.block];
  const auto it = free.emplace(range.offset, range.size).first;
  free_bytes_ += range.size;
  if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
    it->second += next->second;
    free.erase(next);
  }
  if (it != free.begin()) {
    if (auto previous = std::prev(it); previous->first + previous->second == it->first) {
      previous->second += it->second;
      free.erase(it);
    }
  }
}

}  // namespace nier::native
