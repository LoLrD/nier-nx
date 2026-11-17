#include "submissions.h"

#include <array>
#include <cstring>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace nier::native {

namespace vk = rex::ui::vulkan;
using rex::graphics::SpirvShaderTranslator;

namespace {

constexpr uint32_t kMaxSetsPerSlot = 16384;

std::array<VkDeviceSize, SpirvShaderTranslator::kConstantBufferCount> ConstantRanges() {
  std::array<VkDeviceSize, SpirvShaderTranslator::kConstantBufferCount> ranges{};
  ranges[SpirvShaderTranslator::kConstantBufferSystem] = sizeof(SpirvShaderTranslator::SystemConstants);
  ranges[SpirvShaderTranslator::kConstantBufferFloatVertex] = Submissions::kFloatConstantsBytes;
  ranges[SpirvShaderTranslator::kConstantBufferFloatPixel] = Submissions::kFloatConstantsBytes;
  ranges[SpirvShaderTranslator::kConstantBufferBoolLoop] = 40 * sizeof(uint32_t);
  ranges[SpirvShaderTranslator::kConstantBufferFetch] = 192 * sizeof(uint32_t);
  return ranges;
}

}  // namespace

Submissions::Submissions(const vk::VulkanDevice* device, VkDescriptorSetLayout constants_layout) : device_(device) {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  const VkDescriptorPoolSize constants_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                            kSlots * SpirvShaderTranslator::kConstantBufferCount};
  VkDescriptorPoolCreateInfo constants_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  constants_info.maxSets = kSlots;
  constants_info.poolSizeCount = 1;
  constants_info.pPoolSizes = &constants_size;
  dfn.vkCreateDescriptorPool(d, &constants_info, nullptr, &constants_pool_);
  const auto ranges = ConstantRanges();
  for (Slot& slot : slots_) {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = device_->queue_family_graphics_compute();
    dfn.vkCreateCommandPool(d, &pool, nullptr, &slot.pool);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = slot.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 2;
    VkCommandBuffer buffers[2];
    dfn.vkAllocateCommandBuffers(d, &allocate, buffers);
    slot.upload = buffers[0];
    slot.main = buffers[1];
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    dfn.vkCreateFence(d, &fence, nullptr, &slot.fence);
    const std::array<VkDescriptorPoolSize, 2> sizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxSetsPerSlot * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSetsPerSlot * 4},
    }};
    VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.maxSets = kMaxSetsPerSlot;
    descriptors.poolSizeCount = uint32_t(sizes.size());
    descriptors.pPoolSizes = sizes.data();
    dfn.vkCreateDescriptorPool(d, &descriptors, nullptr, &slot.descriptors);
    vk::util::CreateDedicatedAllocationBuffer(device_, kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                              vk::util::MemoryPurpose::kUpload, slot.uniforms, slot.uniform_memory,
                                              &slot.uniform_memory_type);
    void* mapping = nullptr;
    dfn.vkMapMemory(d, slot.uniform_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    slot.uniform_mapping = static_cast<uint8_t*>(mapping);
    VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set.descriptorPool = constants_pool_;
    set.descriptorSetCount = 1;
    set.pSetLayouts = &constants_layout;
    dfn.vkAllocateDescriptorSets(d, &set, &slot.constants_set);
    std::array<VkDescriptorBufferInfo, SpirvShaderTranslator::kConstantBufferCount> infos{};
    std::array<VkWriteDescriptorSet, SpirvShaderTranslator::kConstantBufferCount> writes{};
    for (uint32_t i = 0; i < infos.size(); ++i) {
      infos[i] = {slot.uniforms, 0, ranges[i]};
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.constants_set, i, 0, 1,
                   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &infos[i], nullptr};
    }
    dfn.vkUpdateDescriptorSets(d, uint32_t(writes.size()), writes.data(), 0, nullptr);
  }
}

Submissions::~Submissions() {
  WaitIdle();
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  for (Slot& slot : slots_) {
    dfn.vkDestroyBuffer(d, slot.uniforms, nullptr);
    dfn.vkFreeMemory(d, slot.uniform_memory, nullptr);
    dfn.vkDestroyDescriptorPool(d, slot.descriptors, nullptr);
    dfn.vkDestroyFence(d, slot.fence, nullptr);
    dfn.vkDestroyCommandPool(d, slot.pool, nullptr);
  }
  dfn.vkDestroyDescriptorPool(d, constants_pool_, nullptr);
}

void Submissions::Begin() {
  if (open_) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  Slot& slot = slots_[current_ % kSlots];
  if (slot.submission > completed_) {
    Wait(slot.submission);
  }
  dfn.vkResetFences(d, 1, &slot.fence);
  dfn.vkResetCommandPool(d, slot.pool, 0);
  dfn.vkResetDescriptorPool(d, slot.descriptors, 0);
  slot.uniform_used = 0;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.upload, &begin);
  dfn.vkBeginCommandBuffer(slot.main, &begin);
  open_ = true;
}

bool Submissions::HasUniformSpace(VkDeviceSize bytes) const {
  return slots_[current_ % kSlots].uniform_used + bytes <= kUniformBytes;
}

bool Submissions::Uniform(const void* data, size_t size, size_t reserve, uint32_t& offset) {
  Slot& slot = slots_[current_ % kSlots];
  const VkDeviceSize alignment = device_->properties().minUniformBufferOffsetAlignment;
  const VkDeviceSize start = rex::align(slot.uniform_used, alignment);
  if (start + reserve > kUniformBytes) {
    return false;
  }
  std::memcpy(slot.uniform_mapping + start, data, size);
  slot.uniform_used = start + reserve;
  offset = uint32_t(start);
  return true;
}

VkDescriptorSet Submissions::Allocate(VkDescriptorSetLayout layout) {
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = slots_[current_ % kSlots].descriptors;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (device_->functions().vkAllocateDescriptorSets(device_->device(), &allocate, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return set;
}

void Submissions::Submit() {
  if (!open_) {
    return;
  }
  const auto& dfn = device_->functions();
  Slot& slot = slots_[current_ % kSlots];
  vk::util::FlushMappedMemoryRange(device_, slot.uniform_memory, slot.uniform_memory_type, 0, kUniformBytes,
                                   slot.uniform_used);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(slot.upload, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
  dfn.vkEndCommandBuffer(slot.upload);
  dfn.vkEndCommandBuffer(slot.main);
  const VkCommandBuffer buffers[2] = {slot.upload, slot.main};
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 2;
  submit.pCommandBuffers = buffers;
  {
    auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, slot.fence) != VK_SUCCESS) {
      REXLOG_ERROR("[native] vkQueueSubmit failed");
    }
  }
  slot.submission = current_;
  ++current_;
  open_ = false;
}

void Submissions::Wait(uint64_t submission) {
  if (submission <= completed_) {
    return;
  }
  if (open_ && submission >= current_) {
    Submit();
  }
  for (Slot& slot : slots_) {
    if (slot.submission == submission) {
      device_->functions().vkWaitForFences(device_->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
      completed_ = submission;
      return;
    }
  }
}

bool Submissions::Poll() {
  uint64_t done = completed_;
  for (Slot& slot : slots_) {
    if (slot.submission > done &&
        device_->functions().vkGetFenceStatus(device_->device(), slot.fence) == VK_SUCCESS) {
      done = slot.submission;
    }
  }
  const bool changed = done != completed_;
  completed_ = done;
  return changed;
}

void Submissions::WaitIdle() {
  if (open_) {
    Submit();
  }
  uint64_t last = 0;
  for (const Slot& slot : slots_) {
    last = std::max(last, slot.submission);
  }
  Wait(last);
}

}  // namespace nier::native
