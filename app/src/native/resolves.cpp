#include "resolves.h"

#include <algorithm>
#include <array>

#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

#include "shaders/depth_to_float_cs.h"
#include "texture_loader.h"

namespace nier::native {

namespace vk = rex::ui::vulkan;

namespace {

constexpr VkDeviceSize kReadbackBufferBytes = 256 * 1024;
constexpr uint32_t kMaxDepthSets = 64;

struct DepthConstants {
  int32_t source_offset[2];
  int32_t dest_offset[2];
  int32_t size[2];
  int32_t scale[2];
};

uint64_t ImageKey(uint32_t address, VkFormat format, uint32_t width, uint32_t height) {
  const std::array<uint32_t, 4> fields = {address, uint32_t(format), width, height};
  return XXH3_64bits(fields.data(), sizeof(fields));
}

}  // namespace

Resolves::Resolves(const vk::VulkanDevice* device, RenderTargets& targets, Textures& textures, uint8_t* physical_base)
    : device_(device), targets_(targets), textures_(textures), physical_base_(physical_base) {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = uint32_t(bindings.size());
  set_info.pBindings = bindings.data();
  dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &depth_set_layout_);
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DepthConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &depth_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &depth_layout_);
  const VkShaderModule module = vk::util::CreateShaderModule(device_, depth_to_float_cs, sizeof(depth_to_float_cs));
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                         module, "main", nullptr};
  pipeline_info.layout = depth_layout_;
  dfn.vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &depth_pipeline_);
  dfn.vkDestroyShaderModule(d, module, nullptr);
  const std::array<VkDescriptorPoolSize, 2> sizes = {{
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxDepthSets},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxDepthSets},
  }};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kMaxDepthSets;
  pool_info.poolSizeCount = uint32_t(sizes.size());
  pool_info.pPoolSizes = sizes.data();
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
    if (image.storage_view) {
      dfn.vkDestroyImageView(d, image.storage_view, nullptr);
    }
    dfn.vkDestroyImageView(d, image.view, nullptr);
    dfn.vkDestroyImage(d, image.image, nullptr);
    dfn.vkFreeMemory(d, image.memory, nullptr);
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
    RenderTargets::Target* source = targets_.Get(command_buffer, plan.source);
    if (source) {
      DestImage& dest = Dest(command_buffer, plan, host->format);
      if (plan.depth) {
        CopyDepth(command_buffer, plan, *source, dest);
        ++stats_.depth_copies;
      } else {
        CopyColor(command_buffer, plan, *source, dest);
        ++stats_.copies;
      }
      FullBarrier(device_, command_buffer);
      textures_.AddResolveImage(plan.dest_address, dest.format, dest.width, dest.height, dest.view);
      if (plan.readback) {
        Readback(command_buffer, plan, dest, submission);
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

Resolves::DestImage& Resolves::Dest(VkCommandBuffer command_buffer, const ResolvePlan& plan, VkFormat format) {
  const uint32_t width = std::max(plan.dest_pitch, plan.dest_x + plan.dest_width);
  const uint32_t height = std::max(plan.dest_texture_height, plan.dest_y + plan.dest_height);
  const uint64_t key = ImageKey(plan.dest_address, format, width, height);
  latest_by_address_[plan.dest_address] = key;
  auto [it, inserted] = images_.try_emplace(key);
  DestImage& dest = it->second;
  if (!inserted) {
    return dest;
  }
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  dest.format = format;
  dest.width = width;
  dest.height = height;
  const bool storage = format == VK_FORMAT_R32_SFLOAT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
  vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, dest.image,
                                           dest.memory);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = dest.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCreateImageView(d, &view, nullptr, &dest.view);
  if (storage) {
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dfn.vkCreateImageView(d, &view, nullptr, &dest.storage_view);
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
  return dest;
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
  blit.srcOffsets[0] = {int32_t(plan.source_x), int32_t(plan.source_y), 0};
  blit.srcOffsets[1] = {int32_t(source_right), int32_t(source_bottom), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[0] = {int32_t(plan.dest_x), int32_t(plan.dest_y), 0};
  blit.dstOffsets[1] = {int32_t(plan.dest_x + plan.dest_width * (source_right - plan.source_x) / plan.source_width),
                        int32_t(plan.dest_y + plan.dest_height * (source_bottom - plan.source_y) / plan.source_height),
                        1};
  const bool scaled = plan.source_width != plan.dest_width || plan.source_height != plan.dest_height;
  device_->functions().vkCmdBlitImage(command_buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, dest.image,
                                      VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                                      scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void Resolves::CopyDepth(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                         const DestImage& dest) {
  const auto& dfn = device_->functions();
  const VkDescriptorSet set = DepthDescriptorSet(source.sampled_view, dest.storage_view);
  if (set == VK_NULL_HANDLE) {
    return;
  }
  DepthConstants constants{{int32_t(plan.source_x), int32_t(plan.source_y)},
                           {int32_t(plan.dest_x), int32_t(plan.dest_y)},
                           {int32_t(plan.dest_width), int32_t(plan.dest_height)},
                           {int32_t(plan.source_width / std::max(plan.dest_width, 1u)),
                            int32_t(plan.source_height / std::max(plan.dest_height, 1u))}};
  dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline_);
  dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, depth_layout_, 0, 1, &set, 0, nullptr);
  dfn.vkCmdPushConstants(command_buffer, depth_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  dfn.vkCmdDispatch(command_buffer, (plan.dest_width + 7) / 8, (plan.dest_height + 7) / 8, 1);
}

VkDescriptorSet Resolves::DepthDescriptorSet(VkImageView source, VkImageView dest) {
  const uint64_t key = XXH3_64bits_withSeed(&source, sizeof(source), uint64_t(dest));
  auto [it, inserted] = depth_sets_.try_emplace(key, VK_NULL_HANDLE);
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
    return VK_NULL_HANDLE;
  }
  const VkDescriptorImageInfo source_info{depth_sampler_, source, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo dest_info{VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL};
  std::array<VkWriteDescriptorSet, 2> writes{};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, it->second, 0, 0, 1,
               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &source_info, nullptr, nullptr};
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, it->second, 1, 0, 1,
               VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dest_info, nullptr, nullptr};
  dfn.vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  return it->second;
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
  pending_.push_back({submission, buffer, plan});
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
  std::erase_if(pending_, [&](const PendingReadback& pending) {
    if (pending.submission > submission) {
      return false;
    }
    const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, pending.buffer.memory, 0,
                                    VK_WHOLE_SIZE};
    dfn.vkInvalidateMappedMemoryRanges(device_->device(), 1, &range);
    const ResolvePlan& plan = pending.plan;
    WriteTiled(static_cast<const uint8_t*>(pending.buffer.mapping), plan.dest_x, plan.dest_y, plan.dest_width,
               plan.dest_height, rex::align(plan.dest_pitch, 32u), plan.dest_endian, plan.dest_swap,
               physical_base_ + plan.dest_address);
    free_readbacks_.push_back(pending.buffer);
    return true;
  });
}

VkImage Resolves::FindImage(uint32_t address, uint32_t& width, uint32_t& height) const {
  const auto key = latest_by_address_.find(address);
  if (key == latest_by_address_.end()) {
    return VK_NULL_HANDLE;
  }
  const DestImage& image = images_.at(key->second);
  width = image.width;
  height = image.height;
  return image.image;
}

}  // namespace nier::native
