#include "mirror_sync.h"

#include <algorithm>

#include <rex/hash.h>

namespace nier::native {

MirrorSync::MirrorSync(uint32_t small_range_bytes) : small_range_bytes_(small_range_bytes) {}

void MirrorSync::BeginFrame() { ++frame_; }

uint64_t MirrorSync::Hash(const uint8_t* physical_base, uint64_t key) {
  return XXH3_64bits(physical_base + uint32_t(key >> 32), uint32_t(key));
}

MirrorDecision MirrorSync::Use(uint32_t address, uint32_t size, const uint8_t* physical_base, uint64_t submission) {
  const uint64_t key = Key(address, size);
  auto [it, inserted] = ranges_.try_emplace(key);
  Range& range = it->second;
  if (!inserted && !range.stale && size > small_range_bytes_ && range.checked_frame == frame_) {
    Record(range, key, submission);
    return {MirrorAction::kNone, 0};
  }
  range.checked_frame = frame_;
  const uint64_t hash = Hash(physical_base, key);
  if (!inserted && !range.stale && hash == range.hash) {
    Record(range, key, submission);
    return {MirrorAction::kNone, 0};
  }
  const uint64_t wait = PendingWait(key, physical_base);
  range.hash = hash;
  range.stale = false;
  max_size_ = std::max(max_size_, size);
  InvalidateOverlaps(key);
  Record(range, key, submission);
  return {MirrorAction::kCopy, wait};
}

void MirrorSync::InvalidateOverlaps(uint64_t key) {
  const uint32_t begin = uint32_t(key >> 32);
  const uint32_t end = begin + uint32_t(key);
  const uint32_t first = begin > max_size_ ? begin - max_size_ : 0;
  for (auto it = ranges_.lower_bound(Key(first, 0)); it != ranges_.end() && uint32_t(it->first >> 32) < end; ++it) {
    const uint32_t other_begin = uint32_t(it->first >> 32);
    if (it->first != key && other_begin + uint32_t(it->first) > begin) {
      it->second.stale = true;
    }
  }
}

uint64_t MirrorSync::PendingWait(uint64_t key, const uint8_t* physical_base) const {
  const uint32_t begin = uint32_t(key >> 32);
  const uint32_t end = begin + uint32_t(key);
  uint64_t wait = 0;
  for (const Interval& interval : intervals_) {
    if (interval.submission <= completed_ || interval.submission <= wait || interval.end <= begin ||
        interval.begin >= end) {
      continue;
    }
    if (interval.key == key || Hash(physical_base, interval.key) != ranges_.at(interval.key).hash) {
      wait = interval.submission;
    }
  }
  return wait;
}

void MirrorSync::Record(Range& range, uint64_t key, uint64_t submission) {
  if (range.used_submission == submission) {
    return;
  }
  range.used_submission = submission;
  intervals_.push_back({uint32_t(key >> 32), uint32_t(key >> 32) + uint32_t(key), submission, key});
}

void MirrorSync::Completed(uint64_t submission) {
  completed_ = std::max(completed_, submission);
  std::erase_if(intervals_, [this](const Interval& interval) { return interval.submission <= completed_; });
}

void MirrorSync::Prune(uint64_t unused_frames) {
  std::erase_if(ranges_, [&](const auto& entry) {
    const Range& range = entry.second;
    return range.used_submission <= completed_ && range.checked_frame + unused_frames < frame_;
  });
}

}  // namespace nier::native
