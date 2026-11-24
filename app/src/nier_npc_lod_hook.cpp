#include <rex/cvar.h>
#include <rex/hook.h>

#include "nier_npc_lod.h"

REXCVAR_DEFINE_DOUBLE(nier_npc_update_scale, 1.0, "NieR",
                      "Screen size of a non-essential NPC is multiplied by this before the game decides whether to "
                      "update it this frame (below 1 stops far NPCs sooner)");

REX_EXTERN(__imp__sub_82102370);

REX_HOOK_RAW(sub_82102370) {
  __imp__sub_82102370(ctx, base);
  NierNpcCount(ctx.f1.f64);
  ctx.f1.f64 = NierNpcScaledSize(ctx.f1.f64, REXCVAR_GET(nier_npc_update_scale));
}
