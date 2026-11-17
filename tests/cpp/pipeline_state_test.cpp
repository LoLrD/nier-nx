#include <cstdint>
#include <cstdio>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include "pipeline_state.h"

using namespace rex::graphics;
using namespace nier::native;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static TargetSet ColorAndDepth() {
  TargetSet set{};
  set.color[0] = TargetKey{720, 16, false, 0};
  set.depth = TargetKey{0, 16, true, 0};
  set.scale = {1, 1};
  return set;
}

static HostDraw TriangleList() {
  HostDraw draw{};
  draw.supported = true;
  draw.topology = HostTopology::kTriangleList;
  return draw;
}

int main() {
  auto regs = std::make_unique<RegisterFile>();
  RegisterFile& r = *regs;
  reg::RB_MODECONTROL mode{};
  mode.edram_mode = xenos::EdramMode::kColorDepth;
  r[XE_GPU_REG_RB_MODECONTROL] = mode.value;
  reg::VGT_DRAW_INITIATOR initiator{};
  initiator.prim_type = xenos::PrimitiveType::kTriangleList;
  r[XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator.value;
  {
    Check(ColorBlendFactor(xenos::BlendFactor::kSrcAlpha) == VK_BLEND_FACTOR_SRC_ALPHA &&
              ColorBlendFactor(xenos::BlendFactor::kSrcColor) == VK_BLEND_FACTOR_SRC_COLOR &&
              AlphaBlendFactor(xenos::BlendFactor::kSrcColor) == VK_BLEND_FACTOR_SRC_ALPHA &&
              AlphaBlendFactor(xenos::BlendFactor::kDstColor) == VK_BLEND_FACTOR_DST_ALPHA &&
              ColorBlendFactor(xenos::BlendFactor::kSrcAlphaSaturate) == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE &&
              ColorBlendFactor(xenos::BlendFactor(2)) == VK_BLEND_FACTOR_ZERO,
          "blend_factor_maps");
  }
  {
    reg::RB_DEPTHCONTROL depth{};
    depth.z_enable = 1;
    depth.z_write_enable = 0;
    depth.zfunc = xenos::CompareFunction::kLess;
    r[XE_GPU_REG_RB_DEPTHCONTROL] = depth.value;
    PipelineState s = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(s.depth_test && !s.depth_write && s.depth_compare == VK_COMPARE_OP_LESS, "depth_test_on_when_not_always");
    depth.zfunc = xenos::CompareFunction::kAlways;
    depth.z_write_enable = 1;
    r[XE_GPU_REG_RB_DEPTHCONTROL] = depth.value;
    s = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(s.depth_test && s.depth_write && s.depth_compare == VK_COMPARE_OP_ALWAYS, "depth_test_on_when_write");
    r[XE_GPU_REG_RB_DEPTHCONTROL] = 0;
  }
  {
    reg::RB_DEPTHCONTROL depth{};
    depth.stencil_enable = 1;
    depth.backface_enable = 1;
    depth.stencilfunc = xenos::CompareFunction::kEqual;
    depth.stencilzpass = xenos::StencilOp::kReplace;
    depth.stencilfunc_bf = xenos::CompareFunction::kNotEqual;
    depth.stencilzpass_bf = xenos::StencilOp::kIncrementWrap;
    r[XE_GPU_REG_RB_DEPTHCONTROL] = depth.value;
    PipelineState s = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(s.stencil_test && s.stencil_front[3] == VK_COMPARE_OP_EQUAL && s.stencil_front[1] == VK_STENCIL_OP_REPLACE &&
              s.stencil_back[3] == VK_COMPARE_OP_NOT_EQUAL && s.stencil_back[1] == VK_STENCIL_OP_INCREMENT_AND_WRAP,
          "stencil_back_uses_bf_fields");
    r[XE_GPU_REG_RB_DEPTHCONTROL] = 0;
  }
  {
    reg::PA_SU_SC_MODE_CNTL sc{};
    sc.cull_back = 1;
    sc.face = 1;
    r[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = sc.value;
    PipelineState s = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(s.cull_mode == VK_CULL_MODE_BACK_BIT && s.front_face == VK_FRONT_FACE_CLOCKWISE, "cull_front_back_and_face");
    r[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = 0;
  }
  {
    reg::RB_BLENDCONTROL blend{};
    blend.color_srcblend = xenos::BlendFactor::kSrcAlpha;
    blend.color_destblend = xenos::BlendFactor::kOneMinusSrcAlpha;
    blend.alpha_srcblend = xenos::BlendFactor::kOne;
    blend.alpha_destblend = xenos::BlendFactor::kZero;
    r[XE_GPU_REG_RB_BLENDCONTROL0] = blend.value;
    PipelineState s = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0x7, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(s.attachments[0].write_mask == 0x7 && s.attachments[0].blend_enable &&
              s.attachments[0].src_color == VK_BLEND_FACTOR_SRC_ALPHA &&
              s.attachments[0].dst_color == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          "write_mask_and_blend_from_registers");
    blend.color_srcblend = xenos::BlendFactor::kOne;
    blend.color_destblend = xenos::BlendFactor::kZero;
    r[XE_GPU_REG_RB_BLENDCONTROL0] = blend.value;
    s = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(!s.attachments[0].blend_enable && s.color_formats[0] == VK_FORMAT_R8G8B8A8_UNORM &&
              s.color_formats[1] == VK_FORMAT_UNDEFINED,
          "opaque_blend_disabled");
  }
  {
    PipelineState a = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    PipelineState b = BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF, VK_FORMAT_D32_SFLOAT_S8_UINT);
    HostDraw strip = TriangleList();
    strip.topology = HostTopology::kTriangleStrip;
    strip.primitive_restart = true;
    PipelineState c = BuildPipelineState(r, strip, ColorAndDepth(), 0xF, VK_FORMAT_D24_UNORM_S8_UINT);
    Check(a.Hash() != b.Hash() && a.Hash() == BuildPipelineState(r, TriangleList(), ColorAndDepth(), 0xF,
                                                                 VK_FORMAT_D24_UNORM_S8_UINT).Hash(),
          "hash_differs_by_target_format");
    Check(c.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && c.primitive_restart && a.Hash() != c.Hash(),
          "strip_topology_with_restart");
  }
  return failures ? 1 : 0;
}
