#include "pipeline_state.h"

#include <algorithm>
#include <cstring>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>

namespace nier::native {

using namespace rex::graphics;

namespace {

constexpr VkBlendFactor kColorFactors[32] = {
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_SRC_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_COLOR,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    VK_BLEND_FACTOR_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
};

constexpr VkBlendFactor kAlphaFactors[32] = {
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_DST_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
};

constexpr VkBlendOp kBlendOps[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                    VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD,
                                    VK_BLEND_OP_ADD};

uint8_t Compare(xenos::CompareFunction function) { return uint8_t(VK_COMPARE_OP_NEVER + uint32_t(function)); }

uint8_t Stencil(xenos::StencilOp op) { return uint8_t(VK_STENCIL_OP_KEEP + uint32_t(op)); }

PipelineAttachment Attachment(reg::RB_BLENDCONTROL blend, uint32_t write_mask) {
  PipelineAttachment attachment{};
  attachment.write_mask = uint8_t(write_mask);
  if (!write_mask) {
    return attachment;
  }
  attachment.src_color = uint8_t(ColorBlendFactor(blend.color_srcblend));
  attachment.dst_color = uint8_t(ColorBlendFactor(blend.color_destblend));
  attachment.color_op = uint8_t(kBlendOps[uint32_t(blend.color_comb_fcn) & 7]);
  attachment.src_alpha = uint8_t(AlphaBlendFactor(blend.alpha_srcblend));
  attachment.dst_alpha = uint8_t(AlphaBlendFactor(blend.alpha_destblend));
  attachment.alpha_op = uint8_t(kBlendOps[uint32_t(blend.alpha_comb_fcn) & 7]);
  attachment.blend_enable =
      attachment.src_color != VK_BLEND_FACTOR_ONE || attachment.dst_color != VK_BLEND_FACTOR_ZERO ||
      attachment.color_op != VK_BLEND_OP_ADD || attachment.src_alpha != VK_BLEND_FACTOR_ONE ||
      attachment.dst_alpha != VK_BLEND_FACTOR_ZERO || attachment.alpha_op != VK_BLEND_OP_ADD;
  if (!attachment.blend_enable) {
    attachment.src_color = attachment.dst_color = attachment.src_alpha = attachment.dst_alpha = 0;
  }
  return attachment;
}

uint8_t PolygonMode(reg::PA_SU_SC_MODE_CNTL mode) {
  if (mode.poly_mode != xenos::PolygonModeEnable::kDualMode) {
    return VK_POLYGON_MODE_FILL;
  }
  xenos::PolygonType type = xenos::PolygonType::kTriangles;
  if (!mode.cull_front) {
    type = std::min(type, mode.polymode_front_ptype);
  }
  if (!mode.cull_back) {
    type = std::min(type, mode.polymode_back_ptype);
  }
  return type == xenos::PolygonType::kTriangles ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE;
}

}  // namespace

uint64_t PipelineState::Hash() const { return XXH3_64bits(this, sizeof(*this)); }

bool PipelineState::operator==(const PipelineState& other) const {
  return std::memcmp(this, &other, sizeof(*this)) == 0;
}

VkBlendFactor ColorBlendFactor(xenos::BlendFactor factor) { return kColorFactors[uint32_t(factor) & 31]; }

VkBlendFactor AlphaBlendFactor(xenos::BlendFactor factor) { return kAlphaFactors[uint32_t(factor) & 31]; }

PipelineState BuildPipelineState(const RegisterFile& regs, const HostDraw& draw, const TargetSet& targets,
                                 uint32_t normalized_color_mask, VkFormat depth_format) {
  PipelineState state;
  std::memset(&state, 0, sizeof(state));
  state.topology = uint8_t(draw.topology == HostTopology::kTriangleList ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
  state.primitive_restart = draw.primitive_restart;
  state.depth_clamp = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  const auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  state.polygon_mode = VK_POLYGON_MODE_FILL;
  state.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  if (draw_util::IsPrimitivePolygonal(regs)) {
    state.cull_mode = uint8_t((mode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                              (mode.cull_back ? VK_CULL_MODE_BACK_BIT : 0));
    state.polygon_mode = PolygonMode(mode);
    state.front_face = mode.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  }
  if (targets.depth) {
    state.depth_format = depth_format;
    const auto depth = draw_util::GetNormalizedDepthControl(regs);
    const bool write = depth.z_enable && depth.z_write_enable;
    const xenos::CompareFunction compare = depth.z_enable ? depth.zfunc : xenos::CompareFunction::kAlways;
    state.depth_test = write || compare != xenos::CompareFunction::kAlways;
    state.depth_write = write;
    state.depth_compare = Compare(compare);
    if (depth.stencil_enable) {
      state.stencil_test = 1;
      state.stencil_front[0] = Stencil(depth.stencilfail);
      state.stencil_front[1] = Stencil(depth.stencilzpass);
      state.stencil_front[2] = Stencil(depth.stencilzfail);
      state.stencil_front[3] = Compare(depth.stencilfunc);
      if (draw_util::IsPrimitivePolygonal(regs) && depth.backface_enable) {
        state.stencil_back[0] = Stencil(depth.stencilfail_bf);
        state.stencil_back[1] = Stencil(depth.stencilzpass_bf);
        state.stencil_back[2] = Stencil(depth.stencilzfail_bf);
        state.stencil_back[3] = Compare(depth.stencilfunc_bf);
      } else {
        std::memcpy(state.stencil_back, state.stencil_front, sizeof(state.stencil_back));
      }
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!targets.color[i]) {
      continue;
    }
    state.color_formats[i] = VK_FORMAT_R8G8B8A8_UNORM;
    state.attachments[i] = Attachment(regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]),
                                      (normalized_color_mask >> (4 * i)) & 0xF);
  }
  return state;
}

}  // namespace nier::native
