#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/platform.h>

#include "nier_audio_mix.h"

REXCVAR_DEFINE_BOOL(nier_native_audio_mix, true, "NieR",
                    "Audio: the game's mix-add loop (destination += source * gain) runs natively with NEON instead of "
                    "the recompiled loop");

REX_EXTERN(__imp__sub_824F3F30);

namespace {

uint8_t* GuestPointer(uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

void MarkStores(uint8_t* base, uint32_t address, uint32_t bytes) {
#if defined(REX_STORE_CARDS)
  for (uint32_t page = address >> 12; page <= (address + bytes - 1) >> 12; ++page) {
    base[page] = 0;
  }
#endif
}

}  // namespace

REX_HOOK_RAW(sub_824F3F30) {
  const uint32_t count = ctx.r5.u32;
  if (!count || !REXCVAR_GET(nier_native_audio_mix)) {
    __imp__sub_824F3F30(ctx, base);
    return;
  }
  const uint32_t floats = (((count - 1) >> 3) + 1) * 8;
  const uint32_t destination = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  NierMixAddBigEndian(GuestPointer(base, destination), GuestPointer(base, source), floats, float(ctx.f1.f64));
  MarkStores(base, destination, floats * 4);
  ctx.r3.u64 = destination + floats * 4;
  ctx.r4.u64 = source + floats * 4;
}
