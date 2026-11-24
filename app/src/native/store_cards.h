#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace nier::native {

inline constexpr uint32_t kCardPageShift = 12;
inline constexpr uint32_t kCardPageBytes = 1u << kCardPageShift;
inline constexpr uint32_t kCardPhysicalPages = 0x20000;
inline constexpr uint32_t kCardTableBytes = 0x100000;

struct CardBits {
  uint8_t seen;
  uint8_t recheck;
};

inline constexpr CardBits kMirrorCards{0x01, 0x02};
inline constexpr CardBits kTextureCards{0x04, 0x08};

struct PageRun {
  uint32_t first;
  uint32_t count;
};

void MarkCards(uint8_t* table, uint32_t guest_address, uint32_t bytes);
void MarkPhysicalCards(uint8_t* table, uint32_t physical_address, uint32_t bytes);

class StoreCards {
 public:
  StoreCards(uint8_t* table, CardBits bits) : table_(table), bits_(bits) {}

  bool Take(uint32_t physical_address, uint32_t bytes, std::vector<PageRun>& runs);
  bool Clean(uint32_t first_page, uint32_t count) const { return RunClean(first_page, count); }
  void NextInterval();

  uint64_t taken_pages() const { return taken_pages_; }
  uint64_t rechecked_pages() const { return rechecked_pages_; }
  uint64_t checked_pages() const { return checked_pages_; }

 private:
  bool RunClean(uint32_t first, uint32_t count) const;
  bool PageClean(uint32_t page) const;
  void TakePage(uint32_t page);

  uint8_t* table_;
  CardBits bits_;
  std::vector<uint32_t> taken_;
  uint64_t taken_pages_ = 0;
  uint64_t rechecked_pages_ = 0;
  uint64_t checked_pages_ = 0;
};

class PageSerials {
 public:
  PageSerials() : serials_(kCardPhysicalPages, 0) {}

  void Mark(const std::vector<PageRun>& runs, uint64_t serial);
  void MarkPages(uint32_t first_page, uint32_t count, uint64_t serial);
  uint64_t Newest(uint32_t physical_address, uint32_t bytes) const;
  uint64_t NewestPages(uint32_t first_page, uint32_t count) const;

 private:
  std::vector<uint64_t> serials_;
};

class PageReaders {
 public:
  void Record(uint32_t physical_address, uint32_t bytes, uint64_t submission);
  uint64_t Pending(uint32_t first_page, uint32_t count, uint64_t completed) const;

 private:
  PageSerials readers_;
  std::unordered_set<uint64_t> recorded_;
  uint64_t recorded_submission_ = 0;
};

}  // namespace nier::native
