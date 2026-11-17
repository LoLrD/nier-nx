#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rex::graphics::vulkan {

constexpr uint32_t kEdramDumpRecordTiles = 2048;

struct EdramDumpSpan {
  const void* render_target;
  uint32_t pitch_tiles;
  uint32_t row_length_used;
  uint32_t row_first;
  uint32_t rows;
  uint32_t row_first_start;
  uint32_t row_last_end;
  uint32_t start_tiles;
  uint32_t end_tiles;

  bool operator==(const EdramDumpSpan&) const = default;

  bool Overlaps(const EdramDumpSpan& other) const {
    const uint32_t a_start = start_tiles % kEdramDumpRecordTiles;
    const uint32_t a_end = a_start + (end_tiles - start_tiles);
    const uint32_t b_start = other.start_tiles % kEdramDumpRecordTiles;
    const uint32_t b_end = b_start + (other.end_tiles - other.start_tiles);
    for (uint32_t shift : {0u, kEdramDumpRecordTiles}) {
      if (a_start < b_end + shift && b_start + shift < a_end) {
        return true;
      }
      if (b_start < a_end + shift && a_start + shift < b_end) {
        return true;
      }
    }
    return false;
  }
};

class EdramDumpRecords {
 public:
  bool Contains(const EdramDumpSpan& span) const {
    return std::find(spans_.begin(), spans_.end(), span) != spans_.end();
  }

  void Add(const EdramDumpSpan& span) {
    std::erase_if(spans_, [&span](const EdramDumpSpan& old) { return old.Overlaps(span); });
    spans_.push_back(span);
  }

  void Clear() { spans_.clear(); }

 private:
  std::vector<EdramDumpSpan> spans_;
};

}  // namespace rex::graphics::vulkan
