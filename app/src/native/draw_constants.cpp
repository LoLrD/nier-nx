#include "draw_constants.h"

#include <bit>
#include <cstring>

#include <rex/graphics/registers.h>

namespace nier::native {

using namespace rex::graphics;
using Translator = SpirvShaderTranslator;

namespace {

uint32_t DrawFlags(const RegisterFile& regs, const HostDraw& draw) {
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  uint32_t flags = 0;
  if (draw.index_source == IndexSource::kGuestDma) {
    flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (draw.index_32bit) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    if (draw.primitive_restart) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (vte.vtx_xy_fmt) {
    flags |= Translator::kSysFlag_XYDividedByW;
  }
  if (vte.vtx_z_fmt) {
    flags |= Translator::kSysFlag_ZDividedByW;
  }
  if (vte.vtx_w0_fmt) {
    flags |= Translator::kSysFlag_WNotReciprocal;
  }
  if (draw_util::IsPrimitivePolygonal(regs)) {
    flags |= Translator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= Translator::kSysFlag_PrimitiveLine;
  }
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= Translator::kSysFlag_DepthFloat24;
  }
  const xenos::CompareFunction alpha_function =
      colorcontrol.alpha_test_enable ? colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_function) << Translator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format ==
        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  return flags;
}

}  // namespace

void FillSystemConstants(const RegisterFile& regs, const HostDraw& draw, const draw_util::ViewportInfo& viewport,
                         const TextureConstantInfo& textures, Translator::SystemConstants& out) {
  std::memset(&out, 0, sizeof(out));
  out.flags = DrawFlags(regs, draw);
  if (draw.index_source == IndexSource::kGuestDma) {
    out.vertex_index_load_address = draw.guest_index_address;
    out.vertex_index_endian = draw.index_endian;
    if (draw.primitive_restart) {
      out.vertex_index_reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    }
  } else {
    out.vertex_index_endian = xenos::Endian::kNone;
  }
  out.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  out.vertex_index_min = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  out.vertex_index_max = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (!clip.clip_disable) {
    float* plane = out.user_clip_planes[0];
    for (uint32_t index = 0; index < 6; ++index) {
      if (clip.ucp_ena & (1u << index)) {
        std::memcpy(plane, &regs[XE_GPU_REG_PA_CL_UCP_0_X + index * 4], 4 * sizeof(float));
        plane += 4;
      }
    }
  }
  out.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  out.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  for (uint32_t i = 0; i < 3; ++i) {
    out.ndc_scale[i] = viewport.ndc_scale[i];
    out.ndc_offset[i] = viewport.ndc_offset[i];
  }
  for (uint32_t index = 0; index < 32; ++index) {
    if (!(textures.used_mask & (1u << index))) {
      continue;
    }
    out.texture_swizzled_signs[index >> 2] |= uint32_t(textures.swizzled_signs[index]) << (8 * (index & 3));
    out.texture_swizzles[index >> 1] |= uint32_t(textures.host_swizzles[index] & 0xFFF) << (12 * (index & 1));
  }
  out.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  const auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  out.alpha_to_mask = colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1u << 8) : 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const auto color = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    int32_t bias = color.color_exp_bias;
    if (color.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      bias -= 5;
    }
    out.color_exp_bias[i] = std::bit_cast<float>(uint32_t(0x3F800000 + (bias << 23)));
  }
}

uint32_t PackFloatConstants(const RegisterFile& regs, const Shader::ConstantRegisterMap& map, bool pixel,
                            uint8_t* out) {
  const uint32_t base = pixel ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X;
  uint32_t size = 0;
  for (uint32_t block = 0; block < 4; ++block) {
    uint64_t bits = map.float_bitmap[block];
    while (bits) {
      const uint32_t bit = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      std::memcpy(out + size, &regs[base + (block << 8) + (bit << 2)], 4 * sizeof(float));
      size += 4 * sizeof(float);
    }
  }
  if (!size) {
    std::memset(out, 0, 16);
    size = 16;
  }
  return size;
}

void CopyBoolLoopConstants(const RegisterFile& regs, uint32_t out[40]) {
  std::memcpy(out, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 40 * sizeof(uint32_t));
}

void CopyFetchConstants(const RegisterFile& regs, uint32_t out[192]) {
  std::memcpy(out, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 192 * sizeof(uint32_t));
}

}  // namespace nier::native
