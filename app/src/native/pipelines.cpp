#include "pipelines.h"

#include <array>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

namespace nier::native {

using rex::graphics::SpirvShaderTranslator;

namespace {

constexpr VkShaderStageFlags kGuestStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

VkDescriptorSetLayout CreateSetLayout(const rex::ui::vulkan::VulkanDevice* device,
                                      const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
  VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  device->functions().vkCreateDescriptorSetLayout(device->device(), &info, nullptr, &layout);
  return layout;
}

VkFormat StencilFormat(VkFormat depth) {
  return depth == VK_FORMAT_D24_UNORM_S8_UINT || depth == VK_FORMAT_D32_SFLOAT_S8_UINT ? depth : VK_FORMAT_UNDEFINED;
}

}  // namespace

Pipelines::Pipelines(const rex::ui::vulkan::VulkanDevice* device)
    : device_(device),
      shared_memory_binding_count_(
          1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(device->properties().maxStorageBufferRange)) {
  shared_memory_layout_ =
      CreateSetLayout(device_, {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_memory_binding_count_, kGuestStages,
                                 nullptr}});
  std::vector<VkDescriptorSetLayoutBinding> constants(SpirvShaderTranslator::kConstantBufferCount);
  for (uint32_t i = 0; i < constants.size(); ++i) {
    constants[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, kGuestStages, nullptr};
  }
  constants[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  constants[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  constants_layout_ = CreateSetLayout(device_, constants);
}

Pipelines::~Pipelines() {
  const auto& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (auto& [hash, entries] : pipelines_) {
    for (Entry& entry : entries) {
      dfn.vkDestroyPipeline(device, entry.pipeline, nullptr);
    }
  }
  for (auto& [key, layout] : layouts_) {
    dfn.vkDestroyPipelineLayout(device, layout, nullptr);
  }
  for (auto& [key, layout] : texture_layouts_) {
    dfn.vkDestroyDescriptorSetLayout(device, layout, nullptr);
  }
  dfn.vkDestroyDescriptorSetLayout(device, constants_layout_, nullptr);
  dfn.vkDestroyDescriptorSetLayout(device, shared_memory_layout_, nullptr);
}

VkDescriptorSetLayout Pipelines::TextureSetLayout(bool pixel, uint32_t textures, uint32_t samplers) {
  const uint64_t key = uint64_t(pixel) << 63 | uint64_t(textures) << 32 | samplers;
  auto [it, inserted] = texture_layouts_.try_emplace(key, VK_NULL_HANDLE);
  if (inserted) {
    const VkShaderStageFlags stage = pixel ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < textures + samplers; ++i) {
      bindings.push_back({i, i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage,
                          nullptr});
    }
    it->second = CreateSetLayout(device_, bindings);
  }
  return it->second;
}

VkPipelineLayout Pipelines::Layout(uint32_t vs_textures, uint32_t vs_samplers, uint32_t ps_textures,
                                   uint32_t ps_samplers) {
  const uint64_t key =
      uint64_t(vs_textures) | uint64_t(vs_samplers) << 16 | uint64_t(ps_textures) << 32 | uint64_t(ps_samplers) << 48;
  auto [it, inserted] = layouts_.try_emplace(key, VK_NULL_HANDLE);
  if (inserted) {
    std::array<VkDescriptorSetLayout, SpirvShaderTranslator::kDescriptorSetCount> sets{};
    sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = shared_memory_layout_;
    sets[SpirvShaderTranslator::kDescriptorSetConstants] = constants_layout_;
    sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = TextureSetLayout(false, vs_textures, vs_samplers);
    sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = TextureSetLayout(true, ps_textures, ps_samplers);
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = uint32_t(sets.size());
    info.pSetLayouts = sets.data();
    device_->functions().vkCreatePipelineLayout(device_->device(), &info, nullptr, &it->second);
  }
  return it->second;
}

VkPipeline Pipelines::Get(const PipelineState& state, VkPipelineLayout layout) {
  auto& entries = pipelines_[state.Hash() ^ uint64_t(layout)];
  for (const Entry& entry : entries) {
    if (entry.layout == layout && entry.state == state) {
      return entry.pipeline;
    }
  }
  const VkPipeline pipeline = Create(state, layout);
  if (pipeline != VK_NULL_HANDLE) {
    entries.push_back({state, layout, pipeline});
    ++pipeline_count_;
  }
  return pipeline;
}

VkPipeline Pipelines::Create(const PipelineState& state, VkPipelineLayout layout) const {
  const auto& properties = device_->properties();
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  uint32_t stage_count = 0;
  stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                           VK_SHADER_STAGE_VERTEX_BIT, state.vertex_module, "main", nullptr};
  if (state.pixel_module != VK_NULL_HANDLE) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                             VK_SHADER_STAGE_FRAGMENT_BIT, state.pixel_module, "main", nullptr};
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(state.topology);
  input_assembly.primitiveRestartEnable = state.primitive_restart;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterization.depthClampEnable = state.depth_clamp && properties.depthClamp;
  rasterization.polygonMode =
      properties.fillModeNonSolid ? VkPolygonMode(state.polygon_mode) : VK_POLYGON_MODE_FILL;
  rasterization.cullMode = state.cull_mode;
  rasterization.frontFace = VkFrontFace(state.front_face);
  rasterization.depthBiasEnable = state.depth_format != VK_FORMAT_UNDEFINED;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = state.depth_test;
  depth_stencil.depthWriteEnable = state.depth_write;
  depth_stencil.depthCompareOp = state.depth_test ? VkCompareOp(state.depth_compare) : VK_COMPARE_OP_ALWAYS;
  depth_stencil.stencilTestEnable = state.stencil_test;
  depth_stencil.front = {VkStencilOp(state.stencil_front[0]), VkStencilOp(state.stencil_front[1]),
                         VkStencilOp(state.stencil_front[2]), VkCompareOp(state.stencil_front[3]), 0, 0, 0};
  depth_stencil.back = {VkStencilOp(state.stencil_back[0]), VkStencilOp(state.stencil_back[1]),
                        VkStencilOp(state.stencil_back[2]), VkCompareOp(state.stencil_back[3]), 0, 0, 0};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (state.color_formats[i] != VK_FORMAT_UNDEFINED) {
      color_count = i + 1;
    }
  }
  std::array<VkPipelineColorBlendAttachmentState, 4> attachments{};
  std::array<VkFormat, 4> color_formats{};
  for (uint32_t i = 0; i < color_count; ++i) {
    const PipelineAttachment& a = state.attachments[i];
    color_formats[i] = VkFormat(state.color_formats[i]);
    attachments[i].colorWriteMask = a.write_mask;
    attachments[i].blendEnable = a.blend_enable;
    attachments[i].srcColorBlendFactor = VkBlendFactor(a.src_color);
    attachments[i].dstColorBlendFactor = VkBlendFactor(a.dst_color);
    attachments[i].colorBlendOp = VkBlendOp(a.color_op);
    attachments[i].srcAlphaBlendFactor = VkBlendFactor(a.src_alpha);
    attachments[i].dstAlphaBlendFactor = VkBlendFactor(a.dst_alpha);
    attachments[i].alphaBlendOp = VkBlendOp(a.alpha_op);
  }
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = color_count;
  blend.pAttachments = attachments.data();
  const std::array<VkDynamicState, 7> dynamic_states = {
      VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  VkPipelineRenderingCreateInfoKHR rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  rendering.colorAttachmentCount = color_count;
  rendering.pColorAttachmentFormats = color_formats.data();
  rendering.depthAttachmentFormat = VkFormat(state.depth_format);
  rendering.stencilAttachmentFormat = StencilFormat(VkFormat(state.depth_format));
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages.data();
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (device_->functions().vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                                     &pipeline) != VK_SUCCESS) {
    REXLOG_ERROR("[native] could not create a graphics pipeline");
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

}  // namespace nier::native
