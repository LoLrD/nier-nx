#include "mirror_sync.h"

#include <algorithm>

namespace nier::native {

MirrorRoute RouteMirrorCopy(const MirrorDecision& decision, uint64_t current, uint32_t size, uint32_t stage_limit,
                            bool can_stage) {
  if (decision.action == MirrorAction::kNone) {
    return MirrorRoute::kNone;
  }
  if (!decision.wait_submission) {
    return MirrorRoute::kInPlace;
  }
  if (!can_stage || size > stage_limit) {
    return MirrorRoute::kWait;
  }
  return decision.wait_submission >= current ? MirrorRoute::kFlushThenStage : MirrorRoute::kStage;
}

MirrorSync::MirrorSync(uint32_t small_range_bytes) : small_range_bytes_(small_range_bytes) {}

void MirrorSync::BeginFrame() { ++frame_; }

HashSpan MirrorSync::Span(const uint8_t* physical_base, uint64_t key) {
  return {physical_base + uint32_t(key >> 32), uint32_t(key)};
}

MirrorDecision MirrorSync::Use(uint32_t address, uint32_t size, const uint8_t* physical_base, uint64_t submission) {
  const uint64_t key = Key(address, size);
  auto [it, inserted] = RangesFor(size).try_emplace(key);
  Range& range = it->second;
  if (!inserted && !range.stale && size > small_range_bytes_ && range.checked_frame == frame_) {
    Record(range, key, submission);
    return {MirrorAction::kNone, 0};
  }
  range.checked_frame = frame_;
  const HashSpan span = Span(physical_base, key);
  if (!inserted && !range.stale) {
    const PhasedHash::Result result = range.content.Check({&span, 1});
    hashed_bytes_ += result.hashed_bytes;
    sampled_checks_ += result.sampled;
    if (!result.changed) {
      Record(range, key, submission);
      return {MirrorAction::kNone, 0};
    }
    sampled_changes_ += result.sampled;
    possibly_late_ += result.possibly_late;
  } else {
    hashed_bytes_ += range.content.Reset({&span, 1});
  }
  const uint64_t wait = PendingWait(key, physical_base);
  ForceReaders(key);
  range.stale = false;
  if (size > small_range_bytes_) {
    max_large_size_ = std::max(max_large_size_, size);
  }
  InvalidateOverlaps(key);
  Record(range, key, submission);
  return {MirrorAction::kCopy, wait};
}

void MirrorSync::InvalidateOverlaps(uint64_t key) {
  InvalidateOverlaps(small_, key, small_range_bytes_);
  InvalidateOverlaps(large_, key, max_large_size_);
}

void MirrorSync::InvalidateOverlaps(Ranges& ranges, uint64_t key, uint32_t reach) {
  const uint32_t begin = uint32_t(key >> 32);
  const uint32_t end = begin + uint32_t(key);
  const uint32_t first = begin > reach ? begin - reach : 0;
  for (auto it = ranges.lower_bound(Key(first, 0)); it != ranges.end() && uint32_t(it->first >> 32) < end; ++it) {
    ++overlap_visits_;
    const uint32_t other_begin = uint32_t(it->first >> 32);
    if (it->first != key && other_begin + uint32_t(it->first) > begin) {
      it->second.stale = true;
    }
  }
}

uint64_t MirrorSync::PendingWait(uint64_t key, const uint8_t* physical_base) {
  const uint32_t begin = uint32_t(key >> 32);
  const uint32_t end = begin + uint32_t(key);
  uint64_t wait = 0;
  for (const Interval& interval : intervals_) {
    if (interval.submission <= completed_ || interval.submission <= wait || interval.end <= begin ||
        interval.begin >= end) {
      continue;
    }
    if (interval.key == key || interval.forced) {
      wait = interval.submission;
      continue;
    }
    const HashSpan span = Span(physical_base, interval.key);
    hashed_bytes_ += span.bytes;
    if (!RangesFor(span.bytes).at(interval.key).content.Matches({&span, 1})) {
      wait = interval.submission;
    }
  }
  return wait;
}

void MirrorSync::ForceReaders(uint64_t key) {
  for (Interval& interval : intervals_) {
    if (interval.key == key && interval.submission > completed_) {
      interval.forced = true;
    }
  }
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
  auto unused = [&](const auto& entry) {
    const Range& range = entry.second;
    return range.used_submission <= completed_ && range.checked_frame + unused_frames < frame_;
  };
  std::erase_if(small_, unused);
  std::erase_if(large_, unused);
}

}  // namespace nier::native
