#include "submissions.h"

#include <array>
#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
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

Submissions::Submissions(const vk::VulkanDevice* device, VkDescriptorSetLayout constants_layout, bool host_cached)
    : device_(device) {
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
    const std::array<VkDescriptorPoolSize, 5> sizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxSetsPerSlot * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSetsPerSlot * 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
    }};
    VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.maxSets = kMaxSetsPerSlot;
    descriptors.poolSizeCount = uint32_t(sizes.size());
    descriptors.pPoolSizes = sizes.data();
    dfn.vkCreateDescriptorPool(d, &descriptors, nullptr, &slot.descriptors);
    vk::util::CreateDedicatedAllocationBuffer(device_, kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                              host_cached ? vk::util::MemoryPurpose::kReadback
                                                          : vk::util::MemoryPurpose::kUpload,
                                              slot.uniforms, slot.uniform_memory,
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
  SetupTimestamps();
}

void Submissions::SetupTimestamps() {
  const auto& ifn = device_->vulkan_instance()->functions();
  VkPhysicalDeviceProperties properties;
  ifn.vkGetPhysicalDeviceProperties(device_->physical_device(), &properties);
  uint32_t family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(device_->physical_device(), &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(device_->physical_device(), &family_count, families.data());
  const uint32_t bits = families[device_->queue_family_graphics_compute()].timestampValidBits;
  if (!bits || properties.limits.timestampPeriod <= 0.0f) {
    REXLOG_INFO("[native] the queue has no timestamps, GPU time is not measured");
    return;
  }
  timestamp_period_ns_ = properties.limits.timestampPeriod;
#if REX_PLATFORM_SWITCH
  timestamp_period_ns_ *= 625.0 / 384.0;
#endif
  timestamp_mask_ = bits >= 64 ? ~0ull : (1ull << bits) - 1;
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kTimestamps;
  for (Slot& slot : slots_) {
    device_->functions().vkCreateQueryPool(device_->device(), &info, nullptr, &slot.queries);
    slot.marks.reserve(kTimestamps);
    slot.passes.reserve(kTimestamps);
  }
}

Submissions::~Submissions() {
  WaitIdle();
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  for (Slot& slot : slots_) {
    if (slot.queries) {
      dfn.vkDestroyQueryPool(d, slot.queries, nullptr);
    }
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
  CollectTimes(slot);
  dfn.vkResetFences(d, 1, &slot.fence);
  dfn.vkResetCommandPool(d, slot.pool, 0);
  dfn.vkResetDescriptorPool(d, slot.descriptors, 0);
  slot.shared_sets.clear();
  slot.uniform_used = 0;
  repeated_.Clear();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(slot.upload, &begin);
  dfn.vkBeginCommandBuffer(slot.main, &begin);
  if (slot.queries) {
    dfn.vkCmdResetQueryPool(slot.upload, slot.queries, 0, kTimestamps);
    slot.marks.assign(2, GpuCategory::kNone);
    slot.passes.assign(2, 0);
    WriteTimestamp(slot, slot.upload, 0, GpuCategory::kUploads, 0);
  }
  open_ = true;
}

void Submissions::WriteTimestamp(Slot& slot, VkCommandBuffer command_buffer, uint32_t index, GpuCategory category,
                                 uint16_t pass) {
  slot.marks[index] = category;
  slot.passes[index] = pass;
  device_->functions().vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.queries, index);
}

void Submissions::Mark(GpuCategory category, uint16_t pass) {
  Slot& slot = slots_[current_ % kSlots];
  if (!open_ || !slot.queries || (slot.marks.back() == category && slot.passes.back() == pass) ||
      slot.marks.size() + 1 >= kTimestamps) {
    return;
  }
  slot.marks.push_back(category);
  slot.passes.push_back(pass);
  WriteTimestamp(slot, slot.main, uint32_t(slot.marks.size() - 1), category, pass);
}

void Submissions::Interrupt(GpuCategory category) {
  const Slot& slot = slots_[current_ % kSlots];
  if (slot.marks.empty()) {
    return;
  }
  resume_category_ = slot.marks.back();
  resume_pass_ = slot.passes.back();
  Mark(category);
}

void Submissions::Resume() { Mark(resume_category_, resume_pass_); }

void Submissions::CollectTimes(Slot& slot) {
  if (slot.marks.size() < 2) {
    return;
  }
  std::array<uint64_t, kTimestamps> ticks;
  const uint32_t count = uint32_t(slot.marks.size());
  if (device_->functions().vkGetQueryPoolResults(device_->device(), slot.queries, 0, count,
                                                 count * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    AddGpuTimes(ticks.data(), slot.marks.data(), count, timestamp_period_ns_, timestamp_mask_, gpu_ns_);
    AddPassTimes(ticks.data(), slot.passes.data(), count, timestamp_period_ns_, timestamp_mask_, pass_ns_);
  }
  slot.marks.clear();
  slot.passes.clear();
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

bool Submissions::RepeatedUniform(uint32_t stream, const void* data, size_t size, size_t reserve, uint32_t& offset,
                                  bool& reused) {
  reused = repeated_.Find(stream, data, size, offset);
  if (reused) {
    return true;
  }
  if (!Uniform(data, size, reserve, offset)) {
    return false;
  }
  repeated_.Remember(stream, data, size, offset);
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

VkDescriptorSet Submissions::AllocateShared(VkDescriptorSetLayout layout, uint64_t key, bool& fresh) {
  auto [it, inserted] = slots_[current_ % kSlots].shared_sets.try_emplace(key, VK_NULL_HANDLE);
  fresh = inserted;
  if (inserted) {
    it->second = Allocate(layout);
    if (it->second == VK_NULL_HANDLE) {
      slots_[current_ % kSlots].shared_sets.erase(it);
      return VK_NULL_HANDLE;
    }
  }
  return it->second;
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
  if (slot.queries) {
    WriteTimestamp(slot, slot.upload, 1, GpuCategory::kNone, 0);
    if (slot.marks.size() > 2) {
      slot.marks.push_back(GpuCategory::kNone);
      slot.passes.push_back(0);
      WriteTimestamp(slot, slot.main, uint32_t(slot.marks.size() - 1), GpuCategory::kNone, 0);
    }
  }
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
