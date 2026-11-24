#include "resolves.h"

#include <algorithm>
#include <array>
#include <bit>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include "pipeline_report.h"
#include "shaders/depth_to_float_ps.h"
#include "shaders/depth_to_float_vs.h"
#include "texture_loader.h"

namespace nier::native {

namespace vk = rex::ui::vulkan;

namespace {

constexpr VkDeviceSize kReadbackBufferBytes = 256 * 1024;
constexpr uint32_t kMaxDepthSets = 64;

bool HasRedAndBlue(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return true;
    default:
      return false;
  }
}

struct DepthConstants {
  int32_t source_offset[2];
  int32_t dest_offset[2];
  int32_t scale_shift;
  int32_t down_shift;
};

uint64_t ImageKey(uint32_t address, VkFormat format, uint32_t width, uint32_t height) {
  const std::array<uint32_t, 4> fields = {address, uint32_t(format), width, height};
  return XXH3_64bits(fields.data(), sizeof(fields));
}

}  // namespace

Resolves::Resolves(const vk::VulkanDevice* device, RenderTargets& targets, Textures& textures,
                   rex::memory::Memory* memory, uint8_t* physical_base)
    : device_(device), targets_(targets), textures_(textures), memory_(memory), physical_base_(physical_base) {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &depth_set_layout_);
  const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DepthConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &depth_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &depth_layout_);
  CreateDepthPipeline();
  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxDepthSets};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kMaxDepthSets;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &size;
  dfn.vkCreateDescriptorPool(d, &pool_info, nullptr, &depth_pool_);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_NEAREST;
  sampler.minFilter = VK_FILTER_NEAREST;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dfn.vkCreateSampler(d, &sampler, nullptr, &depth_sampler_);
}

Resolves::~Resolves() {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  for (auto& [key, image] : images_) {
    Destroy(image);
  }
  for (RetiredImage& retired : retired_) {
    Destroy(retired.image);
  }
  for (const PendingReadback& pending : pending_) {
    free_readbacks_.push_back(pending.buffer);
  }
  for (const ReadbackBuffer& buffer : free_readbacks_) {
    dfn.vkDestroyBuffer(d, buffer.buffer, nullptr);
    dfn.vkFreeMemory(d, buffer.memory, nullptr);
  }
  dfn.vkDestroySampler(d, depth_sampler_, nullptr);
  dfn.vkDestroyDescriptorPool(d, depth_pool_, nullptr);
  dfn.vkDestroyPipeline(d, depth_pipeline_, nullptr);
  dfn.vkDestroyPipelineLayout(d, depth_layout_, nullptr);
  dfn.vkDestroyDescriptorSetLayout(d, depth_set_layout_, nullptr);
}

void Resolves::Resolve(VkCommandBuffer command_buffer, const ResolvePlan& plan, uint64_t submission) {
  targets_.End(command_buffer);
  if (plan.copy) {
    const auto host = HostFormatFor(plan.dest_format);
    targets_.Claim(command_buffer, plan.source, plan.source_x, plan.source_y, plan.source_x + plan.source_width,
                   plan.source_y + plan.source_height, true);
    RenderTargets::Target* source = targets_.Get(command_buffer, plan.source);
    DestImage* dest = source ? Dest(command_buffer, plan, host->format) : nullptr;
    DestImage* raw = dest && plan.depth ? Dest(command_buffer, plan, VK_FORMAT_R8G8B8A8_UNORM) : nullptr;
    if (dest && (raw || !plan.depth)) {
      if (plan.depth) {
        CopyDepth(command_buffer, plan, *source, *dest, *raw);
        FullBarrier(device_, command_buffer);
        shadow_map_resolved_ |= source->shadow_map;
        Publish(plan, *raw, 4, submission);
        ++stats_.depth_copies;
      } else if (const auto last = last_copies_.find(plan.dest_address);
                 last != last_copies_.end() && last->second.source_writes == source->writes &&
                 SameColorCopy(last->second.plan, plan)) {
        ++stats_.repeated;
      } else {
        CopyColor(command_buffer, plan, *source, *dest);
        FullBarrier(device_, command_buffer);
        ++stats_.copies;
      }
      if (plan.depth || plan.readback) {
        last_copies_.erase(plan.dest_address);
      } else {
        last_copies_[plan.dest_address] = {plan, source->writes};
      }
      Publish(plan, *dest, host->bytes_per_block, submission);
      if (PresentableResolve(plan)) {
        presentable_by_address_[plan.dest_address] = dest->key;
      }
      if (plan.readback) {
        Readback(command_buffer, plan, *dest, submission);
      }
    } else {
      ++stats_.skipped;
    }
  } else if (plan.dest_width && plan.dest_height) {
    ++stats_.skipped;
  }
  const VkRect2D rect{{int32_t(plan.source_x), int32_t(plan.source_y)}, {plan.source_width, plan.source_height}};
  if (plan.clear_color) {
    RenderTargets::Target* target = targets_.Get(command_buffer, plan.source);
    if (target) {
      VkRect2D clamped = rect;
      clamped.extent.width = std::min(rect.extent.width, target->extent.width - std::min(plan.source_x, target->extent.width));
      clamped.extent.height =
          std::min(rect.extent.height, target->extent.height - std::min(plan.source_y, target->extent.height));
      if (clamped.extent.width && clamped.extent.height) {
        targets_.ClearColor(command_buffer, plan.source, clamped, plan.color_clear);
        ++stats_.clears;
      }
    }
  }
  if (plan.clear_depth) {
    RenderTargets::Target* target = targets_.Get(command_buffer, plan.depth_target);
    if (target) {
      VkRect2D clamped = rect;
      clamped.extent.width = std::min(rect.extent.width, target->extent.width - std::min(plan.source_x, target->extent.width));
      clamped.extent.height =
          std::min(rect.extent.height, target->extent.height - std::min(plan.source_y, target->extent.height));
      if (clamped.extent.width && clamped.extent.height) {
        targets_.ClearDepth(command_buffer, plan.depth_target, clamped, plan.depth_clear, plan.stencil_clear);
        ++stats_.clears;
      }
    }
  }
}

Resolves::DestImage* Resolves::Dest(VkCommandBuffer command_buffer, const ResolvePlan& plan, VkFormat format) {
  const uint32_t width = std::max(plan.dest_pitch, plan.dest_x + plan.dest_width);
  const uint32_t height = std::max(plan.dest_texture_height, plan.dest_y + plan.dest_height);
  const uint64_t key = ImageKey(plan.dest_address, format, width, height);
  auto [it, inserted] = images_.try_emplace(key);
  DestImage& dest = it->second;
  if (!inserted) {
    return &dest;
  }
  dest.key = key;
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  dest.format = format;
  dest.width = width;
  dest.height = height;
  dest.address = plan.dest_address;
  const bool attachment = format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R8G8B8A8_UNORM;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
  if (!vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, dest.image,
                                                dest.memory, nullptr, &dest.bytes)) {
    REXLOG_ERROR("[native] could not create a {}x{} resolve image", width, height);
    images_.erase(it);
    return nullptr;
  }
  bytes_ += dest.bytes;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = dest.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCreateImageView(d, &view, nullptr, &dest.view);
  if (HasRedAndBlue(format)) {
    view.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
    dfn.vkCreateImageView(d, &view, nullptr, &dest.swapped_view);
    view.components = {};
  }
  if (attachment) {
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dfn.vkCreateImageView(d, &view, nullptr, &dest.attachment_view);
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = dest.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
  const VkClearColorValue zero{};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdClearColorImage(command_buffer, dest.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  FullBarrier(device_, command_buffer);
  return &dest;
}

void Resolves::CopyColor(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                         const DestImage& dest) {
  const uint32_t source_right = std::min(plan.source_x + plan.source_width, source.extent.width);
  const uint32_t source_bottom = std::min(plan.source_y + plan.source_height, source.extent.height);
  if (source_right <= plan.source_x || source_bottom <= plan.source_y) {
    return;
  }
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  const uint32_t shift = source.shift;
  blit.srcOffsets[0] = {int32_t(plan.source_x >> shift), int32_t(plan.source_y >> shift), 0};
  blit.srcOffsets[1] = {int32_t(source_right >> shift), int32_t(source_bottom >> shift), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = {int32_t(plan.dest_x), int32_t(plan.dest_y), 0};
  blit.dstOffsets[1] = {int32_t(plan.dest_x + plan.dest_width * (source_right - plan.source_x) / plan.source_width),
                        int32_t(plan.dest_y + plan.dest_height * (source_bottom - plan.source_y) / plan.source_height),
                        1};
  const bool scaled = shift || plan.source_width != plan.dest_width || plan.source_height != plan.dest_height;
  if (shift) {
    ExtendEdges(command_buffer, source, uint32_t(blit.srcOffsets[0].x), uint32_t(blit.srcOffsets[0].y),
                uint32_t(blit.srcOffsets[1].x), uint32_t(blit.srcOffsets[1].y));
  }
  device_->functions().vkCmdBlitImage(command_buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, dest.image,
                                      VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                                      scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void Resolves::ExtendEdges(VkCommandBuffer command_buffer, const RenderTargets::Target& source, uint32_t left,
                           uint32_t top, uint32_t right, uint32_t bottom) {
  const bool column = right < source.extent.width;
  const bool row = bottom < source.extent.height;
  std::array<VkImageCopy, 3> regions{};
  uint32_t count = 0;
  const auto add = [&](int32_t from_x, int32_t from_y, int32_t to_x, int32_t to_y, uint32_t width, uint32_t height) {
    VkImageCopy& region = regions[count++];
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {from_x, from_y, 0};
    region.dstOffset = {to_x, to_y, 0};
    region.extent = {width, height, 1};
  };
  if (column) {
    add(int32_t(right - 1), int32_t(top), int32_t(right), int32_t(top), 1, bottom - top);
  }
  if (row) {
    add(int32_t(left), int32_t(bottom - 1), int32_t(left), int32_t(bottom), right - left, 1);
  }
  if (column && row) {
    add(int32_t(right - 1), int32_t(bottom - 1), int32_t(right), int32_t(bottom), 1, 1);
  }
  if (!count) {
    return;
  }
  const auto& dfn = device_->functions();
  dfn.vkCmdCopyImage(command_buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, source.image, VK_IMAGE_LAYOUT_GENERAL, count,
                     regions.data());
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                           0, nullptr, 0, nullptr);
}

void Resolves::Publish(const ResolvePlan& plan, DestImage& dest, uint32_t bytes_per_pixel, uint64_t submission) {
  dest.last_frame = frame_;
  dest.last_submission = submission;
  const uint32_t guest_bytes = ResolveGuestBytes(dest.width, dest.height, bytes_per_pixel, plan.dest_address);
  const VkImageView view = plan.dest_swap && !plan.depth && dest.swapped_view ? dest.swapped_view : dest.view;
  textures_.AddResolveImage(plan.dest_address, dest.format, dest.width, dest.height, view, guest_bytes);
}

void Resolves::CopyDepth(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                         const DestImage& dest, const DestImage& raw) {
  const auto& dfn = device_->functions();
  const VkDescriptorSet set = DepthDescriptorSet(source.sampled_view);
  if (set == VK_NULL_HANDLE) {
    return;
  }
  std::array<VkRenderingAttachmentInfoKHR, 2> attachments{};
  for (VkRenderingAttachmentInfoKHR& attachment : attachments) {
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  attachments[0].imageView = dest.attachment_view;
  attachments[1].imageView = raw.attachment_view;
  const VkRect2D area{{int32_t(plan.dest_x), int32_t(plan.dest_y)}, {plan.dest_width, plan.dest_height}};
  VkRenderingInfoKHR rendering{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
  rendering.renderArea = area;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = uint32_t(attachments.size());
  rendering.pColorAttachments = attachments.data();
  const uint32_t scale = std::max(plan.source_width / std::max(plan.dest_width, 1u), 1u);
  const DepthConstants constants{{int32_t(plan.source_x >> source.shift), int32_t(plan.source_y >> source.shift)},
                                 {int32_t(plan.dest_x), int32_t(plan.dest_y)},
                                 int32_t(std::bit_width(scale) - 1),
                                 int32_t(source.shift)};
  const VkViewport viewport{float(plan.dest_x), float(plan.dest_y), float(plan.dest_width), float(plan.dest_height),
                            0.0f, 1.0f};
  dfn.vkCmdBeginRendering(command_buffer, &rendering);
  dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline_);
  dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_layout_, 0, 1, &set, 0, nullptr);
  dfn.vkCmdPushConstants(command_buffer, depth_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
  dfn.vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  dfn.vkCmdSetScissor(command_buffer, 0, 1, &area);
  dfn.vkCmdDraw(command_buffer, 3, 1, 0, 0);
  dfn.vkCmdEndRendering(command_buffer);
}

VkDescriptorSet Resolves::DepthDescriptorSet(VkImageView source) {
  auto [it, inserted] = depth_sets_.try_emplace(source, VK_NULL_HANDLE);
  if (!inserted) {
    return it->second;
  }
  const auto& dfn = device_->functions();
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = depth_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &depth_set_layout_;
  if (dfn.vkAllocateDescriptorSets(device_->device(), &allocate, &it->second) != VK_SUCCESS) {
    REXLOG_ERROR("[native] out of depth resolve descriptor sets");
    depth_sets_.erase(it);
    return VK_NULL_HANDLE;
  }
  const VkDescriptorImageInfo source_info{depth_sampler_, source, VK_IMAGE_LAYOUT_GENERAL};
  const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, it->second, 0, 0, 1,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source_info, nullptr, nullptr};
  dfn.vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
  return it->second;
}

void Resolves::CreateDepthPipeline() {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  const VkShaderModule vertex = vk::util::CreateShaderModule(device_, depth_to_float_vs, sizeof(depth_to_float_vs));
  const VkShaderModule fragment = vk::util::CreateShaderModule(device_, depth_to_float_ps, sizeof(depth_to_float_ps));
  const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {{
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
       nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main",
       nullptr},
  }};
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
  std::array<VkPipelineColorBlendAttachmentState, 2> blend_attachments{};
  for (VkPipelineColorBlendAttachmentState& attachment : blend_attachments) {
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = uint32_t(blend_attachments.size());
  blend.pAttachments = blend_attachments.data();
  const std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  const std::array<VkFormat, 2> formats = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
  VkPipelineRenderingCreateInfoKHR rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  rendering.colorAttachmentCount = uint32_t(formats.size());
  rendering.pColorAttachmentFormats = formats.data();
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.flags = PipelineCaptureFlags(device_);
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
  info.layout = depth_layout_;
  dfn.vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &info, nullptr, &depth_pipeline_);
  dfn.vkDestroyShaderModule(d, fragment, nullptr);
  dfn.vkDestroyShaderModule(d, vertex, nullptr);
}

void Resolves::Readback(VkCommandBuffer command_buffer, const ResolvePlan& plan, const DestImage& dest,
                        uint64_t submission) {
  if (plan.dest_x + plan.dest_width > dest.width || plan.dest_y + plan.dest_height > dest.height) {
    return;
  }
  ReadbackBuffer buffer = AcquireReadbackBuffer();
  if (!buffer.mapping) {
    return;
  }
  VkBufferImageCopy region{};
  region.bufferRowLength = plan.dest_width;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(plan.dest_x), int32_t(plan.dest_y), 0};
  region.imageExtent = {plan.dest_width, plan.dest_height, 1};
  device_->functions().vkCmdCopyImageToBuffer(command_buffer, dest.image, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1,
                                              &region);
  FullBarrier(device_, command_buffer);
  PendingRange range{plan.dest_address,
                     ResolveGuestBytes(plan.dest_pitch, plan.dest_y + plan.dest_height, 4, plan.dest_address), 0};
  range.guest_hash = XXH3_64bits(physical_base_ + range.address, range.bytes);
  pending_.push_back({submission, buffer, plan, range});
  ++stats_.readbacks;
}

Resolves::ReadbackBuffer Resolves::AcquireReadbackBuffer() {
  if (!free_readbacks_.empty()) {
    ReadbackBuffer buffer = free_readbacks_.back();
    free_readbacks_.pop_back();
    return buffer;
  }
  ReadbackBuffer buffer;
  if (!vk::util::CreateDedicatedAllocationBuffer(device_, kReadbackBufferBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 vk::util::MemoryPurpose::kReadback, buffer.buffer, buffer.memory)) {
    REXLOG_ERROR("[native] could not create a readback buffer");
    return {};
  }
  device_->functions().vkMapMemory(device_->device(), buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapping);
  return buffer;
}

void Resolves::Completed(uint64_t submission) {
  const auto& dfn = device_->functions();
  size_t done = 0;
  for (; done < pending_.size() && pending_[done].submission <= submission; ++done) {
    const PendingReadback& pending = pending_[done];
    const VkMappedMemoryRange mapped{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, pending.buffer.memory, 0,
                                     VK_WHOLE_SIZE};
    dfn.vkInvalidateMappedMemoryRanges(device_->device(), 1, &mapped);
    const ResolvePlan& plan = pending.plan;
    uint8_t* guest = physical_base_ + pending.range.address;
    if (ShouldWriteBack(pending.range, XXH3_64bits(guest, pending.range.bytes))) {
      WriteTiled(static_cast<const uint8_t*>(pending.buffer.mapping), plan.dest_x, plan.dest_y, plan.dest_width,
                 plan.dest_height, rex::align(plan.dest_pitch, 32u), plan.dest_endian, plan.dest_swap, guest);
      memory_->MarkStoreCards(0xA0000000u + pending.range.address, pending.range.bytes);
      const uint64_t written = XXH3_64bits(guest, pending.range.bytes);
      for (size_t later = done + 1; later < pending_.size(); ++later) {
        CarryWrittenHash(pending_[later].range, pending.range, written);
      }
      textures_.RefreshResolveHash(pending.range.address);
    } else {
      ++stats_.stale_readbacks;
    }
    free_readbacks_.push_back(pending.buffer);
  }
  pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(done));
  std::erase_if(retired_, [&](RetiredImage& retired) {
    if (retired.submission > submission) {
      return false;
    }
    Destroy(retired.image);
    return true;
  });
}

void Resolves::Evict(uint64_t unused_frames, uint64_t submission) {
  std::erase_if(images_, [&](auto& entry) {
    DestImage& image = entry.second;
    if (image.last_frame + unused_frames >= frame_) {
      return false;
    }
    const uint64_t retire = std::max(image.last_submission, submission);
    last_copies_.erase(image.address);
    textures_.RemoveResolveImage(image.address, image.format, image.view);
    if (image.swapped_view) {
      textures_.RemoveResolveImage(image.address, image.format, image.swapped_view);
    }
    if (auto presentable = presentable_by_address_.find(image.address);
        presentable != presentable_by_address_.end() && presentable->second == entry.first) {
      presentable_by_address_.erase(presentable);
    }
    retired_.push_back({image, retire});
    return true;
  });
}

void Resolves::Destroy(DestImage& image) {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  if (image.attachment_view) {
    dfn.vkDestroyImageView(d, image.attachment_view, nullptr);
  }
  if (image.swapped_view) {
    dfn.vkDestroyImageView(d, image.swapped_view, nullptr);
  }
  dfn.vkDestroyImageView(d, image.view, nullptr);
  dfn.vkDestroyImage(d, image.image, nullptr);
  dfn.vkFreeMemory(d, image.memory, nullptr);
  bytes_ -= image.bytes;
}

VkImage Resolves::FindImage(uint32_t address, uint32_t& width, uint32_t& height, VkImageView* view,
                            VkFormat* format) const {
  const auto key = presentable_by_address_.find(address);
  if (key == presentable_by_address_.end()) {
    return VK_NULL_HANDLE;
  }
  const DestImage& image = images_.at(key->second);
  width = image.width;
  height = image.height;
  if (view) {
    *view = image.view;
  }
  if (format) {
    *format = image.format;
  }
  return image.image;
}

}  // namespace nier::native
