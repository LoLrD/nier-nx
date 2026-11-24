#include "scene_upscale.h"

#include <array>
#include <cmath>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include "render_targets.h"
#include "shaders/upscale_alpha_ps.h"
#include "submissions.h"
#include "vk_recorder.h"

namespace nier::native {

namespace shaders {
#include "guest_output_ffx_fsr_easu_ps.h"
#include "guest_output_ffx_fsr_rcas_ps.h"
#include "guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

namespace {

namespace vk = rex::ui::vulkan;

constexpr uint32_t kRectBytes = 16;
constexpr uint32_t kEffectBytes = 16;

struct EasuConstants {
  float input_output_size_ratio[2];
  float input_size_inv[2];
};

struct RcasConstants {
  int32_t output_offset[2];
  float sharpness_post_setup;
  float padding;
};

struct AlphaConstants {
  float inverse_output_size[2];
  float padding[2];
};

constexpr VkColorComponentFlags kColorChannels =
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

uint64_t OutputKey(VkImageView source, VkFormat format, uint32_t width, uint32_t height) {
  return reinterpret_cast<uint64_t>(source) * 0x9E3779B97F4A7C15ull ^
         (uint64_t(format) << 32 | uint64_t(width) << 16 | height);
}

}  // namespace

SceneUpscale::SceneUpscale(const vk::VulkanDevice* device, Submissions& submissions)
    : device_(device), submissions_(submissions) {
  const auto& dfn = Vk(device_);
  const VkDevice d = device_->device();
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dfn.vkCreateSampler(d, &sampler, nullptr, &sampler_);
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
      {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_},
  }};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = uint32_t(bindings.size());
  set_info.pBindings = bindings.data();
  dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &set_layout_);
  const std::array<VkPushConstantRange, 2> ranges = {{
      {VK_SHADER_STAGE_VERTEX_BIT, 0, kRectBytes},
      {VK_SHADER_STAGE_FRAGMENT_BIT, kRectBytes, kEffectBytes},
  }};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = uint32_t(ranges.size());
  layout_info.pPushConstantRanges = ranges.data();
  dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &layout_);
  vertex_ = vk::util::CreateShaderModule(device_, shaders::guest_output_triangle_strip_rect_vs,
                                         sizeof(shaders::guest_output_triangle_strip_rect_vs));
  easu_ = vk::util::CreateShaderModule(device_, shaders::guest_output_ffx_fsr_easu_ps,
                                       sizeof(shaders::guest_output_ffx_fsr_easu_ps));
  rcas_ = vk::util::CreateShaderModule(device_, shaders::guest_output_ffx_fsr_rcas_ps,
                                       sizeof(shaders::guest_output_ffx_fsr_rcas_ps));
  alpha_ = vk::util::CreateShaderModule(device_, upscale_alpha_ps, sizeof(upscale_alpha_ps));
}

SceneUpscale::~SceneUpscale() {
  const auto& dfn = Vk(device_);
  const VkDevice d = device_->device();
  for (auto& [key, output] : outputs_) {
    DestroyOutput(output);
  }
  for (auto& [format, pipelines] : pipelines_) {
    dfn.vkDestroyPipeline(d, pipelines.easu, nullptr);
    dfn.vkDestroyPipeline(d, pipelines.rcas, nullptr);
    dfn.vkDestroyPipeline(d, pipelines.alpha, nullptr);
  }
  dfn.vkDestroyShaderModule(d, alpha_, nullptr);
  dfn.vkDestroyShaderModule(d, rcas_, nullptr);
  dfn.vkDestroyShaderModule(d, easu_, nullptr);
  dfn.vkDestroyShaderModule(d, vertex_, nullptr);
  dfn.vkDestroyPipelineLayout(d, layout_, nullptr);
  dfn.vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
  dfn.vkDestroySampler(d, sampler_, nullptr);
}

VkImageView SceneUpscale::Ready(VkImageView source, VkFormat format, uint32_t output_width, uint32_t output_height,
                                uint64_t version, float sharpness_stops) const {
  const auto it = outputs_.find(OutputKey(source, format, output_width, output_height));
  if (it == outputs_.end() || it->second.source != source || it->second.version != version ||
      it->second.sharpness_stops != sharpness_stops) {
    return VK_NULL_HANDLE;
  }
  return sharpness_stops >= 0.0f ? it->second.rcas_array_view : it->second.easu_array_view;
}

VkImageView SceneUpscale::Upscale(VkCommandBuffer command_buffer, VkImageView source, VkImageView source_plain,
                                  VkFormat format, uint32_t width, uint32_t height, uint32_t output_width,
                                  uint32_t output_height, uint64_t version, float sharpness_stops) {
  Output* output = OutputFor(source, format, output_width, output_height);
  if (!output) {
    return source;
  }
  const bool sharpen = sharpness_stops >= 0.0f;
  const FormatPipelines& pipelines = PipelinesFor(format);
  if (pipelines.easu == VK_NULL_HANDLE || pipelines.alpha == VK_NULL_HANDLE ||
      (sharpen && pipelines.rcas == VK_NULL_HANDLE)) {
    return source;
  }
  const VkDescriptorSet source_set = SourceSet(source_plain);
  const VkDescriptorSet easu_set = sharpen ? SourceSet(output->easu_view) : VK_NULL_HANDLE;
  if (source_set == VK_NULL_HANDLE || (sharpen && easu_set == VK_NULL_HANDLE)) {
    return source;
  }
  if (!output->initialized) {
    Initialize(command_buffer, *output);
    output->initialized = true;
  }
  FullBarrier(device_, command_buffer);
  const EasuConstants easu{{float(width) / float(output_width), float(height) / float(output_height)},
                           {1.0f / float(width), 1.0f / float(height)}};
  Pass(command_buffer, pipelines.easu, source_set, output->easu_view, output_width, output_height, &easu,
       sizeof(easu), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  if (sharpen) {
    FullBarrier(device_, command_buffer);
    const RcasConstants rcas{{0, 0}, std::exp2(-sharpness_stops), 0.0f};
    Pass(command_buffer, pipelines.rcas, easu_set, output->rcas_view, output_width, output_height, &rcas,
         sizeof(rcas), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  }
  FullBarrier(device_, command_buffer);
  const AlphaConstants alpha{{1.0f / float(output_width), 1.0f / float(output_height)}, {0.0f, 0.0f}};
  Pass(command_buffer, pipelines.alpha, source_set, sharpen ? output->rcas_view : output->easu_view, output_width,
       output_height, &alpha, sizeof(alpha), VK_ATTACHMENT_LOAD_OP_LOAD);
  FullBarrier(device_, command_buffer);
  output->source = source;
  output->version = version;
  output->sharpness_stops = sharpness_stops;
  ++runs_;
  return sharpen ? output->rcas_array_view : output->easu_array_view;
}

SceneUpscale::Output* SceneUpscale::OutputFor(VkImageView source, VkFormat format, uint32_t width, uint32_t height) {
  auto [it, inserted] = outputs_.try_emplace(OutputKey(source, format, width, height));
  Output& output = it->second;
  if (!inserted) {
    return &output;
  }
  if (!CreateImage(format, width, height, output.easu, output.easu_memory, output.easu_view, output.easu_array_view) ||
      !CreateImage(format, width, height, output.rcas, output.rcas_memory, output.rcas_view, output.rcas_array_view)) {
    REXLOG_ERROR("[native] could not create {}x{} scene upscale images", width, height);
    DestroyOutput(output);
    outputs_.erase(it);
    return nullptr;
  }
  return &output;
}

bool SceneUpscale::CreateImage(VkFormat format, uint32_t width, uint32_t height, VkImage& image,
                               VkDeviceMemory& memory, VkImageView& view, VkImageView& array_view) {
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, image, memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (Vk(device_).vkCreateImageView(device_->device(), &view_info, nullptr, &view) != VK_SUCCESS) {
    return false;
  }
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  return Vk(device_).vkCreateImageView(device_->device(), &view_info, nullptr, &array_view) == VK_SUCCESS;
}

void SceneUpscale::DestroyOutput(Output& output) {
  const auto& dfn = Vk(device_);
  const VkDevice d = device_->device();
  dfn.vkDestroyImageView(d, output.rcas_array_view, nullptr);
  dfn.vkDestroyImageView(d, output.rcas_view, nullptr);
  dfn.vkDestroyImage(d, output.rcas, nullptr);
  dfn.vkFreeMemory(d, output.rcas_memory, nullptr);
  dfn.vkDestroyImageView(d, output.easu_array_view, nullptr);
  dfn.vkDestroyImageView(d, output.easu_view, nullptr);
  dfn.vkDestroyImage(d, output.easu, nullptr);
  dfn.vkFreeMemory(d, output.easu_memory, nullptr);
  output = {};
}

const SceneUpscale::FormatPipelines& SceneUpscale::PipelinesFor(VkFormat format) {
  auto [it, inserted] = pipelines_.try_emplace(format);
  if (inserted) {
    it->second.easu = CreatePipeline(easu_, format, kColorChannels | VK_COLOR_COMPONENT_A_BIT);
    it->second.rcas = CreatePipeline(rcas_, format, kColorChannels | VK_COLOR_COMPONENT_A_BIT);
    it->second.alpha = CreatePipeline(alpha_, format, VK_COLOR_COMPONENT_A_BIT);
  }
  return it->second;
}

VkPipeline SceneUpscale::CreatePipeline(VkShaderModule fragment, VkFormat format, VkColorComponentFlags write_mask) {
  const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main",
       nullptr},
  }};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask = write_mask;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  VkPipelineRenderingCreateInfoKHR rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = uint32_t(stages.size());
  info.pStages = stages.data();
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (Vk(device_).vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    REXLOG_ERROR("[native] could not create a scene upscale pipeline for format {}", uint32_t(format));
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

VkDescriptorSet SceneUpscale::SourceSet(VkImageView view) {
  const VkDescriptorSet set = submissions_.Allocate(set_layout_);
  if (set == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  const VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
  const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   nullptr,
                                   set,
                                   0,
                                   0,
                                   1,
                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                   &image,
                                   nullptr,
                                   nullptr};
  Vk(device_).vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
  return set;
}

void SceneUpscale::Initialize(VkCommandBuffer command_buffer, const Output& output) {
  std::array<VkImageMemoryBarrier, 2> barriers{};
  const std::array<VkImage, 2> images = {output.easu, output.rcas};
  for (size_t i = 0; i < barriers.size(); ++i) {
    barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barriers[i].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = images[i];
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  Vk(device_).vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
}

void SceneUpscale::Pass(VkCommandBuffer command_buffer, VkPipeline pipeline, VkDescriptorSet set, VkImageView target,
                        uint32_t width, uint32_t height, const void* constants, uint32_t size,
                        VkAttachmentLoadOp load) {
  const auto& dfn = Vk(device_);
  VkRenderingAttachmentInfoKHR color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
  color.imageView = target;
  color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  color.loadOp = load;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfoKHR info{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
  info.renderArea = {{0, 0}, {width, height}};
  info.layerCount = 1;
  info.colorAttachmentCount = 1;
  info.pColorAttachments = &color;
  dfn.vkCmdBeginRendering(command_buffer, &info);
  dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
  const float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
  dfn.vkCmdPushConstants(command_buffer, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, kRectBytes, rect);
  dfn.vkCmdPushConstants(command_buffer, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, kRectBytes, size, constants);
  const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  dfn.vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  const VkRect2D scissor{{0, 0}, {width, height}};
  dfn.vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  dfn.vkCmdDraw(command_buffer, 4, 1, 0, 0);
  dfn.vkCmdEndRendering(command_buffer);
}

}  // namespace nier::native
