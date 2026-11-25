#include "nier_audio_mix.h"

#include <bit>
#include <cmath>
#include <cstring>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

void NierMixAddBigEndian(uint8_t* dst, const uint8_t* src, uint32_t count, float gain) {
  uint32_t i = 0;
#if defined(__ARM_NEON)
  const float32x4_t scale = vdupq_n_f32(gain);
  for (; i + 4 <= count; i += 4) {
    const float32x4_t s = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + i * 4)));
    const float32x4_t d = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(dst + i * 4)));
    vst1q_u8(dst + i * 4, vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(d, s, scale))));
  }
#endif
  for (; i < count; ++i) {
    uint32_t s, d;
    std::memcpy(&s, src + i * 4, 4);
    std::memcpy(&d, dst + i * 4, 4);
    const float sum = std::fmaf(std::bit_cast<float>(__builtin_bswap32(s)), gain, std::bit_cast<float>(__builtin_bswap32(d)));
    const uint32_t out = __builtin_bswap32(std::bit_cast<uint32_t>(sum));
    std::memcpy(dst + i * 4, &out, 4);
  }
}
