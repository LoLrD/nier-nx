#include "guest_mirror.h"

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace nier::native {

namespace vk = rex::ui::vulkan;

GuestMirror::GuestMirror(const vk::VulkanDevice* device, const uint8_t* physical_base)
    : device_(device), physical_base_(physical_base) {
  if (!vk::util::CreateDedicatedAllocationBuffer(device_, kSize,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                 vk::util::MemoryPurpose::kUpload, buffer_, memory_, &memory_type_)) {
    REXLOG_ERROR("[native] could not create the guest memory mirror");
    return;
  }
  void* mapping = nullptr;
  device_->functions().vkMapMemory(device_->device(), memory_, 0, VK_WHOLE_SIZE, 0, &mapping);
  mapping_ = static_cast<uint8_t*>(mapping);
}

GuestMirror::~GuestMirror() {
  const auto& dfn = device_->functions();
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

}  // namespace nier::native
