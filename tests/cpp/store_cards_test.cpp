#include <cstdint>
#include <cstdio>
#include <vector>

#include "store_cards.h"

using nier::native::kMirrorCards;
using nier::native::kTextureCards;
using nier::native::MarkCards;
using nier::native::MarkPhysicalCards;
using nier::native::PageReaders;
using nier::native::PageRun;
using nier::native::PageSerials;
using nier::native::StoreCards;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static uint32_t Pages(const std::vector<PageRun>& runs) {
  uint32_t pages = 0;
  for (const PageRun& run : runs) {
    pages += run.count;
  }
  return pages;
}

int main() {
  std::vector<uint8_t> table(nier::native::kCardTableBytes, 0);
  std::vector<PageRun> runs;
  {
    StoreCards mirror(table.data(), kMirrorCards);
    Check(mirror.Take(0x10000, 0x3000, runs) && runs.size() == 1 && runs[0].first == 0x10 && runs[0].count == 3,
          "untouched_pages_start_dirty");
    runs.clear();
    Check(!mirror.Take(0x10000, 0x3000, runs) && runs.empty(), "taken_pages_are_clean");
    MarkCards(table.data(), 0xA0011004, 4);
    Check(mirror.Take(0x10000, 0x3000, runs) && runs.size() == 1 && runs[0].first == 0x11 && runs[0].count == 1,
          "store_through_a0_view_dirties_its_page");
    runs.clear();
    MarkCards(table.data(), 0xC0012000, 4);
    Check(mirror.Take(0x10800, 0x2000, runs) && Pages(runs) == 1 && runs[0].first == 0x12,
          "store_through_c0_view_dirties_its_page");
    runs.clear();
    MarkCards(table.data(), 0xE0010000, 4);
    Check(mirror.Take(0x10000, 0x3000, runs) && Pages(runs) == 1 && runs[0].first == 0x11,
          "e0_view_is_one_page_above_physical");
    runs.clear();
  }
  {
    StoreCards mirror(table.data(), kMirrorCards);
    StoreCards textures(table.data(), kTextureCards);
    mirror.Take(0x200000, 0x1000, runs);
    textures.Take(0x200000, 0x1000, runs);
    runs.clear();
    MarkCards(table.data(), 0xA0200010, 1);
    Check(mirror.Take(0x200000, 0x1000, runs), "mirror_sees_write");
    runs.clear();
    Check(textures.Take(0x200000, 0x1000, runs), "textures_see_same_write_independently");
    runs.clear();
  }
  {
    StoreCards mirror(table.data(), kMirrorCards);
    mirror.Take(0x300000, 0x2000, runs);
    mirror.NextInterval();
    mirror.Take(0x300000, 0x2000, runs);
    mirror.NextInterval();
    runs.clear();
    Check(!mirror.Take(0x300000, 0x2000, runs), "recheck_happens_once");
    MarkCards(table.data(), 0xA0301000, 4);
    mirror.Take(0x300000, 0x2000, runs);
    runs.clear();
    Check(!mirror.Take(0x300000, 0x2000, runs), "same_interval_after_take_is_clean");
    mirror.NextInterval();
    Check(mirror.Take(0x300000, 0x2000, runs) && Pages(runs) == 1 && runs[0].first == 0x301,
          "page_taken_last_interval_is_rechecked");
    runs.clear();
    mirror.NextInterval();
    Check(!mirror.Take(0x300000, 0x2000, runs), "recheck_does_not_schedule_another");
  }
  {
    StoreCards mirror(table.data(), kMirrorCards);
    mirror.Take(0x400000, 0x1000, runs);
    mirror.NextInterval();
    mirror.Take(0x400000, 0x1000, runs);
    MarkCards(table.data(), 0xA0400000, 4);
    mirror.NextInterval();
    runs.clear();
    Check(mirror.Take(0x400000, 0x1000, runs), "write_before_schedule_is_taken");
    mirror.NextInterval();
    runs.clear();
    Check(mirror.Take(0x400000, 0x1000, runs), "write_before_schedule_gets_its_own_recheck");
    runs.clear();
  }
  {
    StoreCards mirror(table.data(), kMirrorCards);
    mirror.Take(0x500000, 0x10000, runs);
    mirror.NextInterval();
    mirror.Take(0x500000, 0x10000, runs);
    mirror.NextInterval();
    runs.clear();
    MarkCards(table.data(), 0xA0502000, 0x2000);
    MarkCards(table.data(), 0xA050A000, 4);
    Check(mirror.Take(0x500000, 0x10000, runs) && runs.size() == 2 && runs[0].first == 0x502 &&
              runs[0].count == 2 && runs[1].first == 0x50A && runs[1].count == 1,
          "dirty_pages_grouped_into_runs");
    runs.clear();
  }
  {
    StoreCards mirror(table.data(), kMirrorCards);
    mirror.Take(0x600000, 0x1000, runs);
    mirror.NextInterval();
    mirror.Take(0x600000, 0x1000, runs);
    mirror.NextInterval();
    runs.clear();
    MarkPhysicalCards(table.data(), 0x600800, 16);
    Check(mirror.Take(0x600000, 0x1000, runs), "host_physical_write_dirties_page");
    runs.clear();
  }
  {
    PageSerials serials;
    serials.Mark({{0x10, 2}, {0x20, 1}}, 7);
    Check(serials.Newest(0x10000, 0x1000) == 7 && serials.Newest(0x11FFF, 1) == 7, "marked_pages_carry_serial");
    Check(serials.Newest(0x12000, 0xE000) == 0, "unmarked_pages_have_no_serial");
    Check(serials.Newest(0x1F000, 0x2000) == 7, "range_reports_newest_page");
    serials.Mark({{0x11, 1}}, 9);
    Check(serials.Newest(0x10000, 0x2000) == 9 && serials.NewestPages(0x10, 1) == 7, "later_mark_wins_per_page");
  }
  {
    PageReaders readers;
    readers.Record(0x10800, 0x1000, 5);
    Check(readers.Pending(0x10, 1, 4) == 5 && readers.Pending(0x11, 1, 4) == 5, "reader_covers_every_touched_page");
    Check(readers.Pending(0x12, 4, 4) == 0, "untouched_pages_have_no_reader");
    Check(readers.Pending(0x10, 2, 5) == 0, "completed_reader_does_not_block");
    readers.Record(0x10800, 0x1000, 6);
    Check(readers.Pending(0x10, 1, 5) == 6, "newer_submission_replaces_reader");
    for (uint32_t i = 0; i < 6000; ++i) {
      readers.Record(0x1000000 + i * 0x1000, 0x100, 7);
    }
    bool all = true;
    for (uint32_t i = 0; i < 6000; ++i) {
      all &= readers.Pending((0x1000000 >> 12) + i, 1, 6) == 7;
    }
    Check(all, "every_range_recorded_when_the_dedup_table_overflows");
  }
  return failures ? 1 : 0;
}
