#pragma once

#include <array>
#include <cstdint>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>

#include "primitive_setup.h"

namespace nier::native {

struct TextureConstantInfo {
  uint32_t used_mask = 0;
  std::array<uint8_t, 32> swizzled_signs{};
  std::array<uint16_t, 32> host_swizzles{};
};

void FillSystemConstants(const rex::graphics::RegisterFile& regs, const HostDraw& draw,
                         const rex::graphics::draw_util::ViewportInfo& viewport, const TextureConstantInfo& textures,
                         rex::graphics::SpirvShaderTranslator::SystemConstants& out);
uint32_t PackFloatConstants(const rex::graphics::RegisterFile& regs,
                            const rex::graphics::Shader::ConstantRegisterMap& map, bool pixel, uint8_t* out);
void CopyBoolLoopConstants(const rex::graphics::RegisterFile& regs, uint32_t out[40]);
void CopyFetchConstants(const rex::graphics::RegisterFile& regs, uint32_t out[192]);

}  // namespace nier::native
