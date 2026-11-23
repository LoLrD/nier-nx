#include "render_targets.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

#include "shaders/depth_to_color_cs.h"
#include "submissions.h"

namespace nier::native {

namespace vk = rex::ui::vulkan;
namespace xenos = rex::graphics::xenos;
using rex::graphics::xenos::ColorRenderTargetFormat;
using rex::graphics::xenos::DepthRenderTargetFormat;

namespace {

constexpr size_t kLoggedIncompatiblePairs = 20;
constexpr uint32_t kMaxConvertSets = 256;

struct ConvertConstants {
  int32_t source_offset[2];
  int32_t dest_offset[2];
  int32_t size[2];
};

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

RenderTargets::RenderTargets(const vk::VulkanDevice* device, Submissions& submissions)
    : device_(device), submissions_(submissions), depth_format_(ChooseDepthFormat(device)) {
  REXLOG_INFO("[native] depth targets use {}",
              depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
  CreateConverter();
}

void RenderTargets::CreateConverter() {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = uint32_t(bindings.size());
  set_info.pBindings = bindings.data();
  dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &convert_set_layout_);
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ConvertConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &convert_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &convert_layout_);
  const VkShaderModule module = vk::util::CreateShaderModule(device_, depth_to_color_cs, sizeof(depth_to_color_cs));
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                         module, "main", nullptr};
  pipeline_info.layout = convert_layout_;
  dfn.vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &convert_pipeline_);
  dfn.vkDestroyShaderModule(d, module, nullptr);
  const std::array<VkDescriptorPoolSize, 2> sizes = {{
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxConvertSets},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxConvertSets},
  }};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kMaxConvertSets;
  pool_info.poolSizeCount = uint32_t(sizes.size());
  pool_info.pPoolSizes = sizes.data();
  dfn.vkCreateDescriptorPool(d, &pool_info, nullptr, &convert_pool_);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_NEAREST;
  sampler.minFilter = VK_FILTER_NEAREST;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dfn.vkCreateSampler(d, &sampler, nullptr, &convert_sampler_);
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
  dfn.vkDestroySampler(d, convert_sampler_, nullptr);
  dfn.vkDestroyDescriptorPool(d, convert_pool_, nullptr);
  dfn.vkDestroyPipeline(d, convert_pipeline_, nullptr);
  dfn.vkDestroyPipelineLayout(d, convert_layout_, nullptr);
  dfn.vkDestroyDescriptorSetLayout(d, convert_set_layout_, nullptr);
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
  info.usage = (key.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, target.image,
                                                target.memory, nullptr, &target.bytes)) {
    REXLOG_ERROR("[native] could not create a {}x{} render target", target.extent.width, target.extent.height);
    targets_.erase(it);
    return nullptr;
  }
  bytes_ += target.bytes;
  target.owner = uint32_t(owner_keys_.size());
  owner_keys_.push_back(key);
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

bool RenderTargets::Begins(const TargetSet& set) const {
  return !rendering_ || !(Bound{set.color, set.depth} == bound_);
}

void RenderTargets::Begin(VkCommandBuffer command_buffer, const TargetSet& set, uint32_t rows, uint32_t shift,
                          bool shadow_map) {
  if (Begins(set)) {
    End(command_buffer);
    claimed_rows_ = 0;
  } else if (rows <= claimed_rows_) {
    return;
  } else {
    End(command_buffer);
  }
  const Bound bound{set.color, set.depth};
  Own(command_buffer, bound, claimed_rows_, rows, shift, shadow_map);
  claimed_rows_ = std::max(claimed_rows_, rows);
  BeginRendering(command_buffer, bound);
}

void RenderTargets::Own(VkCommandBuffer command_buffer, const Bound& bound, uint32_t from_rows, uint32_t to_rows,
                        uint32_t shift, bool shadow_map) {
  if (bound.depth) {
    if (Target* depth = Get(command_buffer, *bound.depth)) {
      depth->shift = shift;
      depth->shadow_map = shadow_map;
      edram_.Write(*bound.depth, 0, from_rows, depth->extent.width, std::min(to_rows, depth->extent.height),
                   depth->owner, ++sequence_);
    }
  }
  for (const auto& color : bound.color) {
    if (!color) {
      continue;
    }
    if (Target* target = Get(command_buffer, *color)) {
      target->shift = shift;
      Claim(command_buffer, *color, 0, from_rows, target->extent.width, std::min(to_rows, target->extent.height),
            false);
    }
  }
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
      if (color_targets[i]) {
        ++color_targets[i]->writes;
      }
    }
  }
  Target* depth = bound.depth ? Get(command_buffer, *bound.depth) : nullptr;
  if (depth) {
    ++depth->writes;
  }
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
  Discard(key, rect);
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
  Discard(key, rect);
  VkClearAttachment clear{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
  clear.clearValue.depthStencil = {depth, stencil};
  const VkClearRect clear_rect{rect, 0, 1};
  device_->functions().vkCmdClearAttachments(command_buffer, 1, &clear, 1, &clear_rect);
  End(command_buffer);
}

void RenderTargets::Claim(VkCommandBuffer command_buffer, const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1,
                          uint32_t y1, bool newer_than_rendering) {
  Target* target = key.is_depth ? nullptr : Get(command_buffer, key);
  if (!target || target->shift) {
    return;
  }
  std::unordered_map<uint32_t, std::vector<VkImageCopy>> copies;
  for (const ForeignTile& foreign : edram_.ForeignTiles(key, x0, y0, x1, y1, target->owner)) {
    if (newer_than_rendering && foreign.sequence < target->rendered) {
      continue;
    }
    const TargetKey& source_key = owner_keys_[foreign.owner];
    if (!source_key.is_depth && source_key.format != key.format) {
      ++stats_.incompatible_tiles;
      NoteIncompatible(target->owner, foreign.owner);
      continue;
    }
    const auto source = targets_.find(source_key);
    if (source == targets_.end() || source->second.shift) {
      continue;
    }
    const TargetPoint from = TilePosition(source_key, foreign.tile);
    const TargetPoint to = TilePosition(key, foreign.tile);
    if (from.y + xenos::kEdramTileHeightSamples > source->second.extent.height ||
        to.y + xenos::kEdramTileHeightSamples > target->extent.height) {
      continue;
    }
    std::vector<VkImageCopy>& regions = copies[foreign.owner];
    if (!regions.empty()) {
      VkImageCopy& last = regions.back();
      if (last.srcOffset.y == int32_t(from.y) && last.dstOffset.y == int32_t(to.y) &&
          last.srcOffset.x + int32_t(last.extent.width) == int32_t(from.x) &&
          last.dstOffset.x + int32_t(last.extent.width) == int32_t(to.x)) {
        last.extent.width += xenos::kEdramTileWidthSamples;
        ++stats_.transfer_tiles;
        stats_.depth_tiles += source_key.is_depth;
        continue;
      }
    }
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {int32_t(from.x), int32_t(from.y), 0};
    region.dstOffset = {int32_t(to.x), int32_t(to.y), 0};
    region.extent = {xenos::kEdramTileWidthSamples, xenos::kEdramTileHeightSamples, 1};
    regions.push_back(region);
    ++stats_.transfer_tiles;
    stats_.depth_tiles += source_key.is_depth;
  }
  if (!copies.empty()) {
    submissions_.Interrupt(GpuCategory::kTransfers);
    FullBarrier(device_, command_buffer);
    for (const auto& [owner, regions] : copies) {
      const Target& source = targets_.find(owner_keys_[owner])->second;
      if (owner_keys_[owner].is_depth) {
        ConvertDepth(command_buffer, source, *target, regions);
      } else {
        device_->functions().vkCmdCopyImage(command_buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, target->image,
                                            VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()), regions.data());
      }
      stats_.transfer_regions += regions.size();
    }
    ++stats_.transfers;
    ++target->writes;
    FullBarrier(device_, command_buffer);
    submissions_.Resume();
  }
  edram_.Write(key, x0, y0, x1, y1, target->owner, ++sequence_);
  if (!newer_than_rendering) {
    target->rendered = sequence_;
  }
}

void RenderTargets::ConvertDepth(VkCommandBuffer command_buffer, const Target& depth, const Target& color,
                                 const std::vector<VkImageCopy>& regions) {
  const VkDescriptorSet set = ConvertSet(depth, color);
  if (set == VK_NULL_HANDLE) {
    return;
  }
  const auto& dfn = device_->functions();
  dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, convert_pipeline_);
  dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, convert_layout_, 0, 1, &set, 0,
                              nullptr);
  for (const VkImageCopy& region : regions) {
    const ConvertConstants constants{{region.srcOffset.x, region.srcOffset.y},
                                     {region.dstOffset.x, region.dstOffset.y},
                                     {int32_t(region.extent.width), int32_t(region.extent.height)}};
    dfn.vkCmdPushConstants(command_buffer, convert_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                           &constants);
    dfn.vkCmdDispatch(command_buffer, (region.extent.width + 7) / 8, (region.extent.height + 7) / 8, 1);
  }
}

VkDescriptorSet RenderTargets::ConvertSet(const Target& depth, const Target& color) {
  const uint64_t key = uint64_t(depth.owner) << 32 | color.owner;
  if (const auto it = convert_sets_.find(key); it != convert_sets_.end()) {
    return it->second;
  }
  const auto& dfn = device_->functions();
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = convert_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &convert_set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(device_->device(), &allocate, &set) != VK_SUCCESS) {
    REXLOG_ERROR("[native] out of depth-to-colour descriptor sets");
    return VK_NULL_HANDLE;
  }
  const VkDescriptorImageInfo source_info{convert_sampler_, depth.sampled_view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo dest_info{VK_NULL_HANDLE, color.attachment_view, VK_IMAGE_LAYOUT_GENERAL};
  const std::array<VkWriteDescriptorSet, 2> writes = {{
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       &source_info, nullptr, nullptr},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dest_info,
       nullptr, nullptr},
  }};
  dfn.vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  convert_sets_.emplace(key, set);
  return set;
}

void RenderTargets::Discard(const TargetKey& key, const VkRect2D& rect) {
  const auto it = targets_.find(key);
  if (it == targets_.end()) {
    return;
  }
  const uint32_t x0 = (uint32_t(rect.offset.x) + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples *
                      xenos::kEdramTileWidthSamples;
  const uint32_t y0 = (uint32_t(rect.offset.y) + xenos::kEdramTileHeightSamples - 1) /
                      xenos::kEdramTileHeightSamples * xenos::kEdramTileHeightSamples;
  const uint32_t x1 = (uint32_t(rect.offset.x) + rect.extent.width) / xenos::kEdramTileWidthSamples *
                      xenos::kEdramTileWidthSamples;
  const uint32_t y1 = (uint32_t(rect.offset.y) + rect.extent.height) / xenos::kEdramTileHeightSamples *
                      xenos::kEdramTileHeightSamples;
  if (x1 > x0 && y1 > y0) {
    edram_.Write(key, x0, y0, x1, y1, it->second.owner, ++sequence_);
  }
}

void RenderTargets::NoteIncompatible(uint32_t owner, uint32_t foreign) {
  if (logged_incompatible_.size() >= kLoggedIncompatiblePairs || !logged_incompatible_.insert({owner, foreign}).second) {
    return;
  }
  auto describe = [](const TargetKey& key) {
    return fmt::format("{} {:X} w{} f{}", key.is_depth ? "d" : "c", key.base_tiles, ExtentFor(key).width, key.format);
  };
  REXLOG_INFO("[native] edram: [{}] uses tiles last written through [{}] in another format; not transferred",
              describe(owner_keys_[owner]), describe(owner_keys_[foreign]));
}

}  // namespace nier::native
