#include <cstdint>
#include <cstdio>
#include <vector>

#include "pm4_reader.h"

using nier::native::DecodePm4;
using nier::native::RingReader;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static uint32_t Stored(uint32_t value) {
  return __builtin_bswap32(value);
}

int main() {
  {
    const auto h = DecodePm4(0x00012000);
    Check(h.type == 0 && h.count == 2 && h.base_index == 0x2000 && !h.single_register, "decode_type0");
  }
  {
    const auto h = DecodePm4(0xC0036400);
    Check(h.type == 3 && h.opcode == 0x64 && h.count == 4, "decode_type3");
  }
  {
    const auto h = DecodePm4(0);
    Check(h.type == 0 && h.count == 0, "decode_zero_packet");
  }
  {
    const auto h = DecodePm4(0x40000000u | (0x123u << 11) | 0x456u);
    Check(h.type == 1 && h.count == 2 && h.base_index == 0x456 && h.opcode == 0x123, "decode_type1");
  }
  {
    std::vector<uint32_t> ring(8);
    for (uint32_t i = 0; i < 8; ++i) ring[i] = Stored(100 + i);
    RingReader reader(ring.data(), 8, 6, 2);
    Check(reader.Pending() == 4, "ring_wraps_across_end pending");
    const uint32_t a = reader.Read(), b = reader.Read(), c = reader.Read(), d = reader.Read();
    Check(a == 106 && b == 107 && c == 100 && d == 101, "ring_wraps_across_end values");
    Check(reader.Pending() == 0 && reader.position() == 2, "ring_wraps_across_end position");
  }
  {
    const uint8_t bytes[4] = {0x12, 0x34, 0x56, 0x78};
    uint32_t word;
    __builtin_memcpy(&word, bytes, 4);
    RingReader reader(&word, 0, 0, 1);
    Check(reader.Peek() == 0x12345678 && reader.Read() == 0x12345678, "read_swaps_bytes");
  }
  {
    std::vector<uint32_t> buffer = {Stored(1), Stored(2), Stored(3)};
    RingReader reader(buffer.data(), 0, 0, 3);
    reader.Skip(2);
    Check(reader.Pending() == 1 && reader.Read() == 3 && reader.position() == 3, "linear_reader_has_no_wrap");
  }
  {
    std::vector<uint32_t> ring(8);
    RingReader reader(ring.data(), 8, 5, 3);
    Check(reader.Contiguous(3) == ring.data() + 5, "words_before_the_end_are_contiguous");
    Check(reader.Contiguous(4) == nullptr, "words_across_the_end_are_not_contiguous");
    std::vector<uint32_t> buffer(16);
    RingReader linear(buffer.data(), 0, 4, 16);
    Check(linear.Contiguous(12) == buffer.data() + 4, "linear_reader_is_always_contiguous");
  }
  return failures ? 1 : 0;
}
