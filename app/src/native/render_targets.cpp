#include "render_targets.h"

#include <algorithm>
#include <array>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace nier::native {

namespace vk = rex::ui::vulkan;
using rex::graphics::xenos::ColorRenderTargetFormat;
using rex::graphics::xenos::DepthRenderTargetFormat;

namespace {

VkFormat ChooseDepthFormat(const vk::VulkanDevice* device) {
  VkFormatProperties properties{};
  device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
      device->physical_device(), VK_FORMAT_D24_UNORM_S8_UINT, &properties);
  return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
             ? VK_FORMAT_D24_UNORM_S8_UINT
             : VK_FORMAT_D32_SFLOAT_S8_UINT;
}

}  // namespace

void FullBarrier(const vk::VulkanDevice* device, VkCommandBuffer command_buffer) {
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  device->functions().vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                                           nullptr);
}

RenderTargets::RenderTargets(const vk::VulkanDevice* device) : device_(device), depth_format_(ChooseDepthFormat(device)) {
  REXLOG_INFO("[native] depth targets use {}",
              depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
}

RenderTargets::~RenderTargets() {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  for (auto& [key, target] : targets_) {
    if (target.sampled_view != target.attachment_view) {
      dfn.vkDestroyImageView(d, target.sampled_view, nullptr);
    }
    dfn.vkDestroyImageView(d, target.attachment_view, nullptr);
    dfn.vkDestroyImage(d, target.image, nullptr);
    dfn.vkFreeMemory(d, target.memory, nullptr);
  }
}

bool RenderTargets::Supported(const TargetKey& key) {
  return key.is_depth ? key.format == uint32_t(DepthRenderTargetFormat::kD24S8)
                      : key.format == uint32_t(ColorRenderTargetFormat::k_8_8_8_8);
}

RenderTargets::Target* RenderTargets::Get(VkCommandBuffer command_buffer, const TargetKey& key) {
  if (!Supported(key)) {
    return nullptr;
  }
  auto [it, inserted] = targets_.try_emplace(key);
  Target& target = it->second;
  if (!inserted) {
    return &target;
  }
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  target.format = key.is_depth ? depth_format_ : VK_FORMAT_R8G8B8A8_UNORM;
  target.extent = ExtentFor(key);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = target.format;
  info.extent = {target.extent.width, target.extent.height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = (key.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, target.image,
                                           target.memory, nullptr, &target.bytes);
  bytes_ += target.bytes;
  const VkImageAspectFlags aspects =
      key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = target.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = target.format;
  view.subresourceRange = {aspects, 0, 1, 0, 1};
  dfn.vkCreateImageView(d, &view, nullptr, &target.attachment_view);
  target.sampled_view = target.attachment_view;
  if (key.is_depth) {
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dfn.vkCreateImageView(d, &view, nullptr, &target.sampled_view);
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = target.image;
  barrier.subresourceRange = {aspects, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
  const VkImageSubresourceRange range{aspects, 0, 1, 0, 1};
  if (key.is_depth) {
    const VkClearDepthStencilValue zero{};
    dfn.vkCmdClearDepthStencilImage(command_buffer, target.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  } else {
    const VkClearColorValue zero{};
    dfn.vkCmdClearColorImage(command_buffer, target.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  }
  FullBarrier(device_, command_buffer);
  return &target;
}

void RenderTargets::Begin(VkCommandBuffer command_buffer, const TargetSet& set) {
  Bound bound{set.color, set.depth};
  if (rendering_ && bound == bound_) {
    return;
  }
  End(command_buffer);
  BeginRendering(command_buffer, bound);
}

void RenderTargets::BeginRendering(VkCommandBuffer command_buffer, const Bound& bound) {
  std::array<VkRenderingAttachmentInfoKHR, 4> colors{};
  uint32_t color_count = 0;
  uint32_t width = UINT32_MAX;
  uint32_t height = UINT32_MAX;
  std::array<Target*, 4> color_targets{};
  for (uint32_t i = 0; i < 4; ++i) {
    if (bound.color[i]) {
      color_targets[i] = Get(command_buffer, *bound.color[i]);
    }
  }
  Target* depth = bound.depth ? Get(command_buffer, *bound.depth) : nullptr;
  for (uint32_t i = 0; i < 4; ++i) {
    colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
    colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (Target* target = color_targets[i]) {
      colors[i].imageView = target->attachment_view;
      color_count = i + 1;
      width = std::min(width, target->extent.width);
      height = std::min(height, target->extent.height);
    }
  }
  VkRenderingAttachmentInfoKHR depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
  depth_attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  if (depth) {
    depth_attachment.imageView = depth->attachment_view;
    width = std::min(width, depth->extent.width);
    height = std::min(height, depth->extent.height);
  }
  if (width == UINT32_MAX) {
    return;
  }
  VkRenderingInfoKHR info{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
  info.renderArea = {{0, 0}, {width, height}};
  info.layerCount = 1;
  info.colorAttachmentCount = color_count;
  info.pColorAttachments = colors.data();
  info.pDepthAttachment = depth ? &depth_attachment : nullptr;
  info.pStencilAttachment = depth ? &depth_attachment : nullptr;
  device_->functions().vkCmdBeginRendering(command_buffer, &info);
  rendering_ = true;
  bound_ = bound;
  render_area_ = {width, height};
}

void RenderTargets::End(VkCommandBuffer command_buffer) {
  if (!rendering_) {
    return;
  }
  device_->functions().vkCmdEndRendering(command_buffer);
  rendering_ = false;
  FullBarrier(device_, command_buffer);
}

void RenderTargets::ClearColor(VkCommandBuffer command_buffer, const TargetKey& key, const VkRect2D& rect,
                               const float color[4]) {
  if (!Supported(key)) {
    return;
  }
  End(command_buffer);
  Bound bound{};
  bound.color[0] = key;
  BeginRendering(command_buffer, bound);
  VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
  std::copy(color, color + 4, clear.clearValue.color.float32);
  const VkClearRect clear_rect{rect, 0, 1};
  device_->functions().vkCmdClearAttachments(command_buffer, 1, &clear, 1, &clear_rect);
  End(command_buffer);
}

void RenderTargets::ClearDepth(VkCommandBuffer command_buffer, const TargetKey& key, const VkRect2D& rect, float depth,
                               uint32_t stencil) {
  if (!Supported(key)) {
    return;
  }
  End(command_buffer);
  Bound bound{};
  bound.depth = key;
  BeginRendering(command_buffer, bound);
  VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
  clear.clearValue.depthStencil = {depth, stencil};
  const VkClearRect clear_rect{rect, 0, 1};
  device_->functions().vkCmdClearAttachments(command_buffer, 1, &clear, 1, &clear_rect);
  End(command_buffer);
}

}  // namespace nier::native
