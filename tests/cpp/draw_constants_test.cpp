#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include "draw_constants.h"

using namespace rex::graphics;
using namespace nier::native;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static uint32_t Bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

int main() {
  auto regs = std::make_unique<RegisterFile>();
  RegisterFile& r = *regs;
  draw_util::ViewportInfo viewport{};
  viewport.ndc_scale[0] = 2.0f;
  viewport.ndc_offset[1] = -0.5f;
  TextureConstantInfo textures{};
  HostDraw draw{};
  draw.supported = true;
  draw.index_source = IndexSource::kNone;
  draw.index_endian = xenos::Endian::kNone;
  draw.vs_type = Shader::HostVertexShaderType::kVertex;
  {
    SpirvShaderTranslator::SystemConstants c{};
    FillSystemConstants(r, draw, viewport, textures, c);
    Check(Bits(c.color_exp_bias[0]) == 0x3F800000 && Bits(c.color_exp_bias[3]) == 0x3F800000,
          "color_exp_bias_is_one_at_zero_bias");
    Check(((c.flags >> SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift) & 7) == 7,
          "disabled_alpha_test_is_always");
    Check(c.ndc_scale[0] == 2.0f && c.ndc_offset[1] == -0.5f, "ndc_from_viewport");
  }
  {
    r[XE_GPU_REG_VGT_MIN_VTX_INDX] = 3;
    r[XE_GPU_REG_VGT_MAX_VTX_INDX] = 900;
    r[XE_GPU_REG_VGT_INDX_OFFSET] = 7;
    SpirvShaderTranslator::SystemConstants c{};
    FillSystemConstants(r, draw, viewport, textures, c);
    Check(c.vertex_index_min == 3 && c.vertex_index_max == 900 && c.vertex_base_index == 7,
          "vertex_index_range_from_registers");
  }
  {
    HostDraw dma = draw;
    dma.index_source = IndexSource::kGuestDma;
    dma.index_endian = xenos::Endian::k8in16;
    dma.guest_index_address = 0x00ABC000;
    SpirvShaderTranslator::SystemConstants c{};
    FillSystemConstants(r, dma, viewport, textures, c);
    Check(c.vertex_index_endian == xenos::Endian::k8in16 && c.vertex_index_load_address == 0x00ABC000 &&
              (c.flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad),
          "guest_dma_sets_index_endian");
  }
  {
    textures.used_mask = 0b101;
    textures.swizzled_signs[2] = 0x5A;
    textures.host_swizzles[2] = 0x688;
    SpirvShaderTranslator::SystemConstants c{};
    FillSystemConstants(r, draw, viewport, textures, c);
    Check(((c.texture_swizzled_signs[0] >> 16) & 0xFF) == 0x5A, "texture_signs_packed");
    Check((c.texture_swizzles[1] & 0xFFF) == 0x688, "texture_swizzles_packed");
    textures = {};
  }
  {
    Shader::ConstantRegisterMap map{};
    map.float_bitmap[0] = (1ull << 1) | (1ull << 5);
    map.float_count = 2;
    r[XE_GPU_REG_SHADER_CONSTANT_000_X + 1 * 4] = 0x11111111;
    r[XE_GPU_REG_SHADER_CONSTANT_000_X + 5 * 4 + 3] = 0x55555555;
    r[XE_GPU_REG_SHADER_CONSTANT_256_X + 1 * 4] = 0x22222222;
    uint8_t out[256] = {};
    const uint32_t size = PackFloatConstants(r, map, false, out);
    uint32_t words[8];
    std::memcpy(words, out, sizeof(words));
    Check(size == 32 && words[0] == 0x11111111 && words[7] == 0x55555555, "float_constants_packed_by_bitmap");
    PackFloatConstants(r, map, true, out);
    std::memcpy(words, out, sizeof(words));
    Check(words[0] == 0x22222222, "pixel_float_constants_start_at_256");
    Shader::ConstantRegisterMap empty{};
    Check(PackFloatConstants(r, empty, false, out) == 16, "empty_float_constants_are_16_bytes");
  }
  {
    r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] = 0xCAFE0001;
    r[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 191] = 0xCAFE00BF;
    r[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 39] = 0xB0071;
    uint32_t fetch[192];
    uint32_t bools[40];
    CopyFetchConstants(r, fetch);
    CopyBoolLoopConstants(r, bools);
    Check(fetch[0] == 0xCAFE0001 && fetch[191] == 0xCAFE00BF, "fetch_constants_copied");
    Check(bools[39] == 0xB0071, "bool_loop_constants_copied");
  }
  return failures ? 1 : 0;
}
