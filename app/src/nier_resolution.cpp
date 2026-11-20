#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_INT32(nier_resolution_level, -1, "NieR",
                     "Scene resolution level kept fixed: -1 lets the game choose, 0 = 1280x720, 1 = 1024x576, "
                     "2 = 768x432");

REX_EXTERN(__imp__sub_8210A3B0);

namespace {

constexpr uint32_t kLevelOffset = 68;

}  // namespace

REX_HOOK_RAW(sub_8210A3B0) {
  if (const int32_t level = REXCVAR_GET(nier_resolution_level); level >= 0) {
    *reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + kLevelOffset) = __builtin_bswap32(uint32_t(level));
  }
  __imp__sub_8210A3B0(ctx, base);
}
