#include <cstdint>
#include <cstdio>
#include <vector>

#include "mirror_sync.h"

using nier::native::MirrorAction;
using nier::native::MirrorSync;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  std::vector<uint8_t> memory(1 << 20, 0x11);
  const uint8_t* base = memory.data();
  {
    MirrorSync sync;
    auto first = sync.Use(0x100, 0x100, base, 1);
    auto second = sync.Use(0x100, 0x100, base, 1);
    Check(first.action == MirrorAction::kCopy && first.wait_submission == 0, "first_use_copies");
    Check(second.action == MirrorAction::kNone, "unchanged_range_not_copied");
  }
  {
    MirrorSync sync;
    sync.Use(0x1000, 0x20000, base, 1);
    memory[0x1000] = 0x22;
    auto same_frame = sync.Use(0x1000, 0x20000, base, 1);
    sync.BeginFrame();
    auto next_frame = sync.Use(0x1000, 0x20000, base, 2);
    Check(same_frame.action == MirrorAction::kNone, "same_frame_large_range_not_rehashed");
    Check(next_frame.action == MirrorAction::kCopy && next_frame.wait_submission == 1,
          "large_range_rehashed_next_frame_waits_for_reader");
    memory[0x1000] = 0x11;
  }
  {
    MirrorSync sync;
    sync.Use(0x200, 0x40, base, 3);
    memory[0x210] = 0x33;
    auto changed = sync.Use(0x200, 0x40, base, 3);
    Check(changed.action == MirrorAction::kCopy, "small_range_rehashed_every_use");
    Check(changed.wait_submission == 3, "changed_range_used_in_pending_submission_waits");
    memory[0x210] = 0x11;
  }
  {
    MirrorSync sync;
    sync.Use(100, 100, base, 1);
    memory[160] = 0x44;
    auto overlap = sync.Use(150, 100, base, 2);
    Check(overlap.action == MirrorAction::kCopy && overlap.wait_submission == 1, "overlap_with_other_key_waits");
    memory[160] = 0x11;
  }
  {
    MirrorSync sync;
    sync.Use(100, 100, base, 1);
    memory[220] = 0x55;
    auto overlap = sync.Use(150, 100, base, 2);
    Check(overlap.action == MirrorAction::kCopy && overlap.wait_submission == 0,
          "overlap_with_unchanged_range_does_not_wait");
    memory[220] = 0x11;
  }
  {
    MirrorSync sync;
    sync.Use(0x300, 0x40, base, 4);
    sync.Completed(4);
    memory[0x300] = 0x66;
    auto changed = sync.Use(0x300, 0x40, base, 5);
    Check(changed.action == MirrorAction::kCopy && changed.wait_submission == 0, "completed_submission_needs_no_wait");
    memory[0x300] = 0x11;
  }
  {
    MirrorSync sync;
    sync.Use(100, 100, base, 1);
    sync.Completed(1);
    memory[160] = 0x77;
    sync.Use(150, 100, base, 2);
    sync.Completed(2);
    memory[160] = 0x11;
    auto reverted = sync.Use(100, 100, base, 3);
    Check(reverted.action == MirrorAction::kCopy, "overlapping_copy_invalidates_other_keys");
  }
  {
    MirrorSync sync;
    sync.Use(0x400, 0x40, base, 1);
    sync.Use(0x800, 0x40, base, 1);
    sync.Completed(1);
    for (int i = 0; i < 10; ++i) {
      sync.BeginFrame();
    }
    sync.Use(0x800, 0x40, base, 2);
    sync.Prune(5);
    Check(sync.key_count() == 1, "old_keys_pruned");
    sync.Prune(0);
    Check(sync.key_count() == 1, "keys_read_by_pending_submission_kept");
  }
  {
    MirrorSync sync;
    sync.Use(0x10100, 0x40, base, 1);
    sync.Completed(1);
    memory[0x10110] = 0x88;
    sync.BeginFrame();
    sync.Use(0x10000, 0x20000, base, 2);
    sync.Completed(2);
    memory[0x10110] = 0x11;
    auto reverted = sync.Use(0x10100, 0x40, base, 3);
    Check(reverted.action == MirrorAction::kCopy, "large_copy_invalidates_small_keys");
  }
  {
    MirrorSync sync;
    sync.Use(0x10000, 0x20000, base, 1);
    sync.Completed(1);
    memory[0x10110] = 0x99;
    sync.Use(0x10100, 0x40, base, 2);
    sync.Completed(2);
    memory[0x10110] = 0x11;
    sync.BeginFrame();
    auto reverted = sync.Use(0x10000, 0x20000, base, 3);
    Check(reverted.action == MirrorAction::kCopy, "small_copy_invalidates_large_keys");
  }
  {
    MirrorSync sync;
    sync.Use(0, 0x80000, base, 1);
    for (uint32_t i = 0; i < 1000; ++i) {
      sync.Use(0x80000 + i * 0x200, 0x100, base, 1);
    }
    sync.Completed(1);
    memory[0x80000 + 999 * 0x200] = 0xAA;
    const uint64_t before = sync.overlap_visits();
    sync.Use(0x80000 + 999 * 0x200, 0x100, base, 2);
    Check(sync.overlap_visits() - before < 200, "copy_visits_only_nearby_keys");
    memory[0x80000 + 999 * 0x200] = 0x11;
  }
  {
    MirrorSync sync;
    for (uint64_t frame = 1; frame <= 10; ++frame) {
      sync.BeginFrame();
      sync.Use(0x40000, 0x80000, base, frame);
      sync.Completed(frame);
    }
    sync.BeginFrame();
    const uint64_t before = sync.hashed_bytes();
    auto stable = sync.Use(0x40000, 0x80000, base, 11);
    Check(stable.action == MirrorAction::kNone && sync.hashed_bytes() - before < 0x80000 / 4,
          "stable_large_range_sampled");
    sync.Completed(11);
    for (uint32_t i = 0x40000; i < 0xC0000; i += 256) {
      memory[i] = 0xBB;
    }
    sync.BeginFrame();
    auto rewritten = sync.Use(0x40000, 0x80000, base, 12);
    Check(rewritten.action == MirrorAction::kCopy, "rewritten_large_range_copied_next_frame");
    for (uint32_t i = 0x40000; i < 0xC0000; i += 256) {
      memory[i] = 0x11;
    }
  }
  return failures ? 1 : 0;
}
