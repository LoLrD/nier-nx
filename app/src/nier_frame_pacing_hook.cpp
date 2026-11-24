#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/platform.h>

#include "nier_frame_pacing.h"

REXCVAR_DEFINE_INT32(nier_late_frame_threshold, 100, "NieR",
                     "Swap callback: percent of a vblank after which a late frame waits for the next vblank (the "
                     "game uses 0, so every late frame waits; 100 shows a late frame at once; -1 keeps the game's)");
REXCVAR_DEFINE_INT32(nier_max_present_interval, 2, "NieR",
                     "Swap callback: longest present interval in vblanks the game may ask for (2 stops it from "
                     "choosing 20 fps after a slow frame; 0 keeps the game's)");

REX_EXTERN(__imp__sub_8257A4B0);

namespace {

constexpr uint32_t kDevicePointer = 0x82000594;
constexpr uint32_t kVblankCount = 16756;
constexpr uint32_t kLastTarget = 16764;
constexpr uint32_t kFlips = 16776;

uint32_t Load(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  const uint64_t host = uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  const uint64_t host = address;
#endif
  uint32_t value;
  std::memcpy(&value, base + host, sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

REX_HOOK_RAW(sub_8257A4B0) {
  const uint32_t requested = ctx.r3.u32;
  const uint32_t param =
      NierSoftenSwapParam(requested, REXCVAR_GET(nier_late_frame_threshold), REXCVAR_GET(nier_max_present_interval));
  const uint32_t pointer = Load(base, kDevicePointer);
  const uint32_t device = pointer ? Load(base, pointer) : 0;
  const uint32_t last_target = device ? Load(base, device + kLastTarget) : 0;
  const uint32_t flips = device ? Load(base, device + kFlips) : 0;
  ctx.r3.u64 = param;
  __imp__sub_8257A4B0(ctx, base);
  if (device) {
    const uint32_t count = Load(base, device + kVblankCount);
    const bool released = Load(base, device + kFlips) != flips;
    NierCountSwap((requested >> 8) & 0xF, NierClassifySwap(last_target, (param >> 8) & 0xF, count, released));
  }
}
