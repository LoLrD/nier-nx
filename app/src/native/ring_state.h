#pragma once

#include <atomic>
#include <cstdint>

namespace nier::native {

class RingState {
 public:
  void Initialize(uint32_t base, uint32_t size_log2) {
    size_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    base_.store(base, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  void SetWritePointer(uint32_t words) { write_pointer_.store(words, std::memory_order_seq_cst); }

  bool HasWork(uint32_t read) const {
    const uint32_t size = size_words();
    return size && (write_pointer() & (size - 1)) != read;
  }

  bool HasNewWork(uint32_t read, uint32_t seen_write_pointer) const {
    return HasWork(read) && write_pointer() != seen_write_pointer;
  }

  uint32_t base() const { return base_.load(std::memory_order_acquire); }
  uint32_t size_words() const { return size_words_.load(std::memory_order_acquire); }
  uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
  uint32_t write_pointer() const { return write_pointer_.load(std::memory_order_seq_cst); }

 private:
  std::atomic<uint32_t> base_{0};
  std::atomic<uint32_t> size_words_{0};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
};

}  // namespace nier::native
