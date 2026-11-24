#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace nier::native {

struct ArenaRange {
  uint32_t block;
  uint64_t offset;
  uint64_t size;
};

class ArenaRanges {
 public:
  explicit ArenaRanges(uint64_t block_bytes) : block_bytes_(block_bytes) {}

  uint32_t AddBlock();
  std::optional<ArenaRange> Allocate(uint64_t size, uint64_t alignment);
  void Free(const ArenaRange& range);
  uint64_t block_bytes() const { return block_bytes_; }
  uint32_t block_count() const { return uint32_t(blocks_.size()); }
  uint64_t free_bytes() const { return free_bytes_; }

 private:
  uint64_t block_bytes_;
  std::vector<std::map<uint64_t, uint64_t>> blocks_;
  uint64_t free_bytes_ = 0;
};

}  // namespace nier::native
