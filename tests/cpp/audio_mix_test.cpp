#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nier_audio_mix.h"

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static void Put(std::vector<uint8_t>& bytes, size_t index, float value) {
  const uint32_t word = __builtin_bswap32(std::bit_cast<uint32_t>(value));
  std::memcpy(bytes.data() + index * 4, &word, 4);
}

static float Get(const std::vector<uint8_t>& bytes, size_t index) {
  uint32_t word;
  std::memcpy(&word, bytes.data() + index * 4, 4);
  return std::bit_cast<float>(__builtin_bswap32(word));
}

int main() {
  const size_t count = 40;
  std::vector<uint8_t> dst(count * 4 + 8), src(count * 4 + 8);
  for (size_t i = 0; i < count; ++i) {
    Put(dst, i, 0.25f * float(i) - 3.0f);
    Put(src, i, std::sin(float(i)) * 0.7f);
  }
  Put(dst, count, 123.0f);
  std::vector<uint8_t> expected = dst;
  const float gain = 0.3125f;
  for (size_t i = 0; i < count; ++i) {
    Put(expected, i, std::fmaf(Get(src, i), gain, Get(dst, i)));
  }
  NierMixAddBigEndian(dst.data(), src.data(), count, gain);
  bool same = true;
  for (size_t i = 0; i < count; ++i) {
    same &= Get(dst, i) == Get(expected, i);
  }
  Check(same, "each_sample_gets_source_times_gain_added_with_one_rounding");
  Check(Get(dst, count) == 123.0f, "samples_past_the_count_are_untouched");
  return failures ? 1 : 0;
}
