#pragma once

#include <cstdint>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

#include "primitive_setup.h"
#include "target_keys.h"

namespace nier::native {

struct PipelineAttachment {
  uint8_t blend_enable;
  uint8_t src_color;
  uint8_t dst_color;
  uint8_t color_op;
  uint8_t src_alpha;
  uint8_t dst_alpha;
  uint8_t alpha_op;
  uint8_t write_mask;
};

struct PipelineState {
  VkShaderModule vertex_module;
  VkShaderModule pixel_module;
  uint32_t color_formats[4];
  uint32_t depth_format;
  uint8_t topology;
  uint8_t primitive_restart;
  uint8_t depth_clamp;
  uint8_t cull_mode;
  uint8_t front_face;
  uint8_t polygon_mode;
  uint8_t depth_test;
  uint8_t depth_write;
  uint8_t depth_compare;
  uint8_t stencil_test;
  uint8_t stencil_front[4];
  uint8_t stencil_back[4];
  uint8_t padding[2];
  PipelineAttachment attachments[4];

  uint64_t Hash() const;
  bool operator==(const PipelineState& other) const;
};

VkBlendFactor ColorBlendFactor(rex::graphics::xenos::BlendFactor factor);
VkBlendFactor AlphaBlendFactor(rex::graphics::xenos::BlendFactor factor);
PipelineState BuildPipelineState(const rex::graphics::RegisterFile& regs, const HostDraw& draw,
                                 const TargetSet& targets, uint32_t normalized_color_mask, VkFormat depth_format);

}  // namespace nier::native
