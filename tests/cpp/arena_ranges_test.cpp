#include <cstdint>
#include <cstdio>

#include "arena_ranges.h"

using nier::native::ArenaRange;
using nier::native::ArenaRanges;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  {
    ArenaRanges arena(1024);
    Check(!arena.Allocate(16, 16), "no_block_no_allocation");
    Check(arena.AddBlock() == 0, "first_block_is_zero");
    const auto a = arena.Allocate(100, 16);
    Check(a && a->block == 0 && a->offset == 0 && a->size == 100, "first_allocation_starts_the_block");
    const auto b = arena.Allocate(64, 256);
    Check(b && b->block == 0 && b->offset == 256, "allocation_is_aligned");
    const auto c = arena.Allocate(100, 4);
    Check(c && c->block == 0 && c->offset == 100, "alignment_gap_is_reused");
    Check(!arena.Allocate(1024, 1), "too_large_for_the_free_space");
    Check(arena.AddBlock() == 1, "second_block_is_one");
    const auto d = arena.Allocate(1024, 1);
    Check(d && d->block == 1 && d->offset == 0, "full_block_fits_a_new_block");
    arena.Free(*a);
    arena.Free(*c);
    arena.Free(*b);
    const auto whole = arena.Allocate(1024, 1024);
    Check(whole && whole->block == 0 && whole->offset == 0, "freed_ranges_merge_back_into_the_whole_block");
    arena.Free(*whole);
    Check(arena.free_bytes() == 1024, "free_bytes_count_the_free_ranges");
    Check(arena.block_count() == 2, "blocks_are_kept");
  }
  {
    ArenaRanges arena(4096);
    arena.AddBlock();
    const auto a = arena.Allocate(1000, 1);
    const auto b = arena.Allocate(1000, 1);
    const auto c = arena.Allocate(1000, 1);
    arena.Free(*b);
    const auto small = arena.Allocate(500, 1);
    Check(small && small->offset == 1000, "first_fit_takes_the_earliest_hole");
    arena.Free(*a);
    arena.Free(*small);
    const auto merged = arena.Allocate(2000, 1);
    Check(merged && merged->offset == 0, "hole_merges_with_both_neighbours");
    arena.Free(*c);
    arena.Free(*merged);
    Check(arena.free_bytes() == 4096, "everything_free_again");
  }
  return failures ? 1 : 0;
}
