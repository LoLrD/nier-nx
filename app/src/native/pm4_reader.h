#pragma once

#include <cstdint>

namespace nier::native {

struct Pm4Header {
  uint32_t type;
  uint32_t count;
  uint32_t opcode;
  uint32_t base_index;
  bool single_register;
};

inline Pm4Header DecodePm4(uint32_t packet) {
  Pm4Header header{packet >> 30, 0, 0, 0, false};
  switch (header.type) {
    case 0:
      header.count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
      header.base_index = packet & 0x7FFF;
      header.single_register = ((packet >> 15) & 1) != 0;
      break;
    case 1:
      header.count = 2;
      header.base_index = packet & 0x7FF;
      header.opcode = (packet >> 11) & 0x7FF;
      break;
    case 2:
      break;
    default:
      header.count = ((packet >> 16) & 0x3FFF) + 1;
      header.opcode = (packet >> 8) & 0x7F;
      break;
  }
  return header;
}

class RingReader {
 public:
  RingReader(const uint32_t* words_big_endian, uint32_t size_words, uint32_t start, uint32_t end)
      : words_(words_big_endian), mask_(size_words ? size_words - 1 : 0), position_(start), end_(end) {}

  uint32_t Pending() const { return mask_ ? (end_ - position_) & mask_ : end_ - position_; }

  uint32_t Peek() const { return __builtin_bswap32(words_[position_]); }

  uint32_t Read() {
    const uint32_t value = Peek();
    Skip(1);
    return value;
  }

  void Skip(uint32_t words) { position_ = mask_ ? (position_ + words) & mask_ : position_ + words; }

  uint32_t position() const { return position_; }

 private:
  const uint32_t* words_;
  uint32_t mask_;
  uint32_t position_;
  uint32_t end_;
};

}  // namespace nier::native
