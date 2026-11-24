#include "guest_mirror.h"
#include "vk_recorder.h"

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>
#include <rex/ui/vulkan/util.h>

namespace nier::native {

namespace vk = rex::ui::vulkan;

GuestMirror::GuestMirror(const vk::VulkanDevice* device, const uint8_t* physical_base, bool host_cached)
    : device_(device), physical_base_(physical_base) {
  if (!vk::util::CreateDedicatedAllocationBuffer(device_, kSize,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 host_cached ? vk::util::MemoryPurpose::kReadback
                                                             : vk::util::MemoryPurpose::kUpload,
                                                 buffer_, memory_, &memory_type_)) {
    REXLOG_ERROR("[native] could not create the guest memory mirror");
    return;
  }
  void* mapping = nullptr;
  Vk(device_).vkMapMemory(device_->device(), memory_, 0, VK_WHOLE_SIZE, 0, &mapping);
  mapping_ = static_cast<uint8_t*>(mapping);
  const auto& types = device_->memory_types();
  gpu_writable_ = (types.host_coherent >> memory_type_) & 1;
  REXLOG_INFO("[native] guest mirror: memory type {}, {}, {}, GPU copies into it {}", memory_type_,
              (types.host_cached >> memory_type_) & 1 ? "cached" : "uncached",
              gpu_writable_ ? "coherent" : "not coherent", gpu_writable_ ? "on" : "off");
}

GuestMirror::~GuestMirror() {
  const auto& dfn = Vk(device_);
  dfn.vkDestroyBuffer(device_->device(), buffer_, nullptr);
  dfn.vkFreeMemory(device_->device(), memory_, nullptr);
}

void GuestMirror::Copy(uint32_t address, uint32_t size) {
  if (address >= kSize) {
    return;
  }
  size = std::min(size, kSize - address);
  std::memcpy(mapping_ + address, physical_base_ + address, size);
  vk::util::FlushMappedMemoryRange(device_, memory_, memory_type_, address, kSize, size);
}

bool GuestMirror::Stage(VkCommandBuffer upload, uint64_t submission, vk::VulkanUploadBufferPool& staging,
                        uint32_t address, uint32_t size) {
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  uint8_t* mapping = staging.Request(submission, size, 16, source, offset);
  if (!mapping) {
    return false;
  }
  std::memcpy(mapping, physical_base_ + address, size);
  const auto& dfn = Vk(device_);
  if (barrier_submission_ != submission) {
    barrier_submission_ = submission;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn.vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
  }
  const VkBufferCopy region{offset, address, size};
  dfn.vkCmdCopyBuffer(upload, source, buffer_, 1, &region);
  return true;
}

}  // namespace nier::native
