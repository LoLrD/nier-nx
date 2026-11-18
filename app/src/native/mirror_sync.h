#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "phased_hash.h"

namespace nier::native {

enum class MirrorAction { kNone, kCopy };

struct MirrorDecision {
  MirrorAction action;
  uint64_t wait_submission;
};

class MirrorSync {
 public:
  explicit MirrorSync(uint32_t small_range_bytes = 65536);

  void BeginFrame();
  MirrorDecision Use(uint32_t address, uint32_t size, const uint8_t* physical_base, uint64_t submission);
  void Completed(uint64_t submission);
  void Prune(uint64_t unused_frames);
  size_t key_count() const { return small_.size() + large_.size(); }
  uint64_t hashed_bytes() const { return hashed_bytes_; }
  uint64_t overlap_visits() const { return overlap_visits_; }
  uint64_t sampled_checks() const { return sampled_checks_; }
  uint64_t late_changes() const { return late_changes_; }

 private:
  struct Range {
    PhasedHash content;
    uint64_t checked_frame = UINT64_MAX;
    uint64_t used_submission = 0;
    bool stale = false;
  };
  struct Interval {
    uint32_t begin;
    uint32_t end;
    uint64_t submission;
    uint64_t key;
  };

  using Ranges = std::map<uint64_t, Range>;

  static uint64_t Key(uint32_t address, uint32_t size) { return uint64_t(address) << 32 | size; }
  Ranges& RangesFor(uint32_t size) { return size > small_range_bytes_ ? large_ : small_; }
  static HashSpan Span(const uint8_t* physical_base, uint64_t key);
  uint64_t PendingWait(uint64_t key, const uint8_t* physical_base);
  void Record(Range& range, uint64_t key, uint64_t submission);
  void InvalidateOverlaps(uint64_t key);
  void InvalidateOverlaps(Ranges& ranges, uint64_t key, uint32_t reach);

  uint32_t small_range_bytes_;
  uint64_t frame_ = 0;
  uint64_t completed_ = 0;
  Ranges small_;
  Ranges large_;
  uint32_t max_large_size_ = 0;
  std::vector<Interval> intervals_;
  uint64_t hashed_bytes_ = 0;
  uint64_t overlap_visits_ = 0;
  uint64_t sampled_checks_ = 0;
  uint64_t late_changes_ = 0;
};

}  // namespace nier::native
