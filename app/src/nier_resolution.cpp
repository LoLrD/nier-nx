#include <bit>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(nier_resolution_level, -1, "NieR",
                     "Scene resolution level kept fixed: -1 lets the game choose, 0 = 1280x720, 1 = 1024x576, "
                     "2 = 768x432");

REXCVAR_DEFINE_DOUBLE(nier_level2_scale, 0.6, "NieR",
                      "Scene size of resolution level 2 as a fraction of 1280x720 (the game uses 0.6 = 768x432)");

REX_EXTERN(__imp__sub_8210A3B0);
REX_EXTERN(__imp__sub_825928B8);
REX_EXTERN(__imp__sub_82592958);

namespace {

constexpr uint32_t kLevelOffset = 68;
constexpr uint32_t kLevel2ScaleAddress = 0x8209E89C;

void PatchLevel2Scale(uint8_t* base) {
  const uint32_t value = __builtin_bswap32(std::bit_cast<uint32_t>(float(REXCVAR_GET(nier_level2_scale))));
  auto* scale = reinterpret_cast<uint32_t*>(base + kLevel2ScaleAddress);
  if (*scale == value) {
    return;
  }
  auto* heap = rex::system::kernel_state()->memory()->LookupHeap(kLevel2ScaleAddress);
  uint32_t protect = 0;
  heap->Protect(kLevel2ScaleAddress, sizeof(uint32_t),
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &protect);
  *scale = value;
  heap->Protect(kLevel2ScaleAddress, sizeof(uint32_t), protect);
}

}  // namespace

REX_HOOK_RAW(sub_8210A3B0) {
  if (const int32_t level = REXCVAR_GET(nier_resolution_level); level >= 0) {
    *reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + kLevelOffset) = __builtin_bswap32(uint32_t(level));
  }
  __imp__sub_8210A3B0(ctx, base);
}

REX_HOOK_RAW(sub_825928B8) {
  PatchLevel2Scale(base);
  __imp__sub_825928B8(ctx, base);
}

REX_HOOK_RAW(sub_82592958) {
  PatchLevel2Scale(base);
  __imp__sub_82592958(ctx, base);
}
