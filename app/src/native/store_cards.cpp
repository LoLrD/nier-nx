#include "store_cards.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace nier::native {

namespace {

constexpr uint32_t kViewA = 0xA0000;
constexpr uint32_t kViewC = 0xC0000;
constexpr uint32_t kViewE = 0xE0000;
constexpr uint32_t kChunkPages = 64;

bool AllHave(const uint8_t* bytes, uint32_t count, uint8_t bit) {
  const uint64_t mask = uint64_t(bit) * 0x0101010101010101ull;
  uint32_t i = 0;
  for (; i + 8 <= count; i += 8) {
    uint64_t word;
    std::memcpy(&word, bytes + i, sizeof(word));
    if ((word & mask) != mask) {
      return false;
    }
  }
  for (; i < count; ++i) {
    if (!(bytes[i] & bit)) {
      return false;
    }
  }
  return true;
}

}  // namespace

void MarkCards(uint8_t* table, uint32_t guest_address, uint32_t bytes) {
  if (!bytes) {
    return;
  }
  const uint32_t first = guest_address >> kCardPageShift;
  const uint32_t last = uint32_t((uint64_t(guest_address) + bytes - 1) >> kCardPageShift);
  for (uint32_t page = first; page <= last; ++page) {
    std::atomic_ref<uint8_t>(table[page]).store(0, std::memory_order_release);
  }
}

void MarkPhysicalCards(uint8_t* table, uint32_t physical_address, uint32_t bytes) {
  MarkCards(table, 0xA0000000u + physical_address, bytes);
}

bool StoreCards::RunClean(uint32_t first, uint32_t count) const {
  if (!AllHave(table_ + kViewA + first, count, bits_.seen) || !AllHave(table_ + kViewC + first, count, bits_.seen)) {
    return false;
  }
  const uint32_t e_first = std::max(first, 1u);
  return e_first >= first + count || AllHave(table_ + kViewE + e_first - 1, first + count - e_first, bits_.seen);
}

bool StoreCards::PageClean(uint32_t page) const { return RunClean(page, 1); }

void StoreCards::TakePage(uint32_t page) {
  std::atomic_ref<uint8_t> a(table_[kViewA + page]);
  uint8_t old = a.load(std::memory_order_relaxed);
  while (!a.compare_exchange_weak(old, uint8_t((old | bits_.seen) & ~bits_.recheck), std::memory_order_acq_rel,
                                  std::memory_order_relaxed)) {
  }
  bool written = !(old & bits_.seen) && !(old & bits_.recheck);
  const uint8_t c = std::atomic_ref<uint8_t>(table_[kViewC + page]).fetch_or(bits_.seen, std::memory_order_acq_rel);
  written |= !(c & bits_.seen);
  if (page) {
    const uint8_t e =
        std::atomic_ref<uint8_t>(table_[kViewE + page - 1]).fetch_or(bits_.seen, std::memory_order_acq_rel);
    written |= !(e & bits_.seen);
  }
  ++taken_pages_;
  if (written) {
    taken_.push_back(page);
  } else {
    ++rechecked_pages_;
  }
}

bool StoreCards::Take(uint32_t physical_address, uint32_t bytes, std::vector<PageRun>& runs) {
  if (!bytes) {
    return false;
  }
  const uint32_t first = physical_address >> kCardPageShift;
  const uint32_t end =
      std::min(uint32_t((uint64_t(physical_address) + bytes - 1) >> kCardPageShift) + 1, kCardPhysicalPages);
  checked_pages_ += end - first;
  bool any = false;
  for (uint32_t chunk = first; chunk < end; chunk += kChunkPages) {
    const uint32_t count = std::min(kChunkPages, end - chunk);
    if (RunClean(chunk, count)) {
      continue;
    }
    for (uint32_t page = chunk; page < chunk + count; ++page) {
      if (PageClean(page)) {
        continue;
      }
      TakePage(page);
      any = true;
      if (!runs.empty() && runs.back().first + runs.back().count == page) {
        ++runs.back().count;
      } else {
        runs.push_back({page, 1});
      }
    }
  }
  return any;
}

void StoreCards::NextInterval() {
  for (const uint32_t page : taken_) {
    std::atomic_ref<uint8_t> a(table_[kViewA + page]);
    uint8_t old = a.load(std::memory_order_relaxed);
    while ((old & bits_.seen) &&
           !a.compare_exchange_weak(old, uint8_t((old & ~bits_.seen) | bits_.recheck), std::memory_order_acq_rel,
                                    std::memory_order_relaxed)) {
    }
  }
  taken_.clear();
}

void PageSerials::MarkPages(uint32_t first_page, uint32_t count, uint64_t serial) {
  std::fill_n(serials_.begin() + first_page, std::min(count, kCardPhysicalPages - first_page), serial);
}

void PageSerials::Mark(const std::vector<PageRun>& runs, uint64_t serial) {
  for (const PageRun& run : runs) {
    MarkPages(run.first, run.count, serial);
  }
}

uint64_t PageSerials::NewestPages(uint32_t first_page, uint32_t count) const {
  const uint32_t end = std::min(first_page + count, kCardPhysicalPages);
  uint64_t newest = 0;
  for (uint32_t page = first_page; page < end; ++page) {
    newest = std::max(newest, serials_[page]);
  }
  return newest;
}

uint64_t PageSerials::Newest(uint32_t physical_address, uint32_t bytes) const {
  if (!bytes) {
    return 0;
  }
  const uint32_t first = physical_address >> kCardPageShift;
  const uint32_t last = uint32_t((uint64_t(physical_address) + bytes - 1) >> kCardPageShift);
  return NewestPages(first, last - first + 1);
}

void PageReaders::Record(uint32_t physical_address, uint32_t bytes, uint64_t submission) {
  if (!bytes) {
    return;
  }
  if (submission != recorded_submission_) {
    recorded_.clear();
    recorded_submission_ = submission;
  }
  if (!recorded_.insert((uint64_t(physical_address) << 32) | bytes).second) {
    return;
  }
  const uint32_t first = physical_address >> kCardPageShift;
  const uint32_t last = uint32_t((uint64_t(physical_address) + bytes - 1) >> kCardPageShift);
  readers_.MarkPages(first, last - first + 1, submission);
}

uint64_t PageReaders::Pending(uint32_t first_page, uint32_t count, uint64_t completed) const {
  const uint64_t newest = readers_.NewestPages(first_page, count);
  return newest > completed ? newest : 0;
}

}  // namespace nier::native
