#include <bit>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "native/upscale_plan.h"

REXCVAR_DEFINE_INT32(nier_resolution_level, -1, "NieR",
                     "Scene resolution level kept fixed: -1 lets the game choose, 0 = 1280x720, 1 = 1024x576, "
                     "2 = 768x432");

REXCVAR_DEFINE_DOUBLE(nier_level1_scale, 0.8, "NieR",
                      "Scene size of resolution level 1 as a fraction of 1280x720 (the game uses 0.8 = 1024x576)");

REXCVAR_DEFINE_DOUBLE(nier_level2_scale, 0.6, "NieR",
                      "Scene size of resolution level 2 as a fraction of 1280x720 (the game uses 0.6 = 768x432)");

REXCVAR_DEFINE_BOOL(nier_dynamic_resolution, false, "NieR",
                    "The native renderer's GPU governor switches the scene between resolution levels 1 and 2 instead "
                    "of keeping nier_resolution_level");

REX_EXTERN(__imp__sub_8210A3B0);
REX_EXTERN(__imp__sub_825928B8);
REX_EXTERN(__imp__sub_82592958);

namespace {

constexpr uint32_t kLevelOffset = 68;
constexpr uint32_t kLevel1ScaleAddress = 0x8209E898;
constexpr uint32_t kLevel2ScaleAddress = 0x8209E89C;

void PatchScale(uint8_t* base, uint32_t address, double scale) {
  const uint32_t value = __builtin_bswap32(std::bit_cast<uint32_t>(float(scale)));
  auto* stored = reinterpret_cast<uint32_t*>(base + address);
  if (*stored == value) {
    return;
  }
  auto* heap = rex::system::kernel_state()->memory()->LookupHeap(address);
  uint32_t protect = 0;
  heap->Protect(address, sizeof(uint32_t), rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                &protect);
  *stored = value;
  heap->Protect(address, sizeof(uint32_t), protect);
}

void PatchLevelScales(uint8_t* base) {
  PatchScale(base, kLevel1ScaleAddress, REXCVAR_GET(nier_level1_scale));
  PatchScale(base, kLevel2ScaleAddress, REXCVAR_GET(nier_level2_scale));
}

int32_t SceneLevel() {
  if (REXCVAR_GET(nier_dynamic_resolution)) {
    return nier::native::g_scene_reduced.load(std::memory_order_relaxed) ? 2 : 1;
  }
  return REXCVAR_GET(nier_resolution_level);
}

}  // namespace

REX_HOOK_RAW(sub_8210A3B0) {
  if (const int32_t level = SceneLevel(); level >= 0) {
    *reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + kLevelOffset) = __builtin_bswap32(uint32_t(level));
  }
  __imp__sub_8210A3B0(ctx, base);
}

REX_HOOK_RAW(sub_825928B8) {
  PatchLevelScales(base);
  __imp__sub_825928B8(ctx, base);
}

REX_HOOK_RAW(sub_82592958) {
  PatchLevelScales(base);
  __imp__sub_82592958(ctx, base);
}
