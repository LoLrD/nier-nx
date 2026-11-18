#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class GuestMirror {
 public:
  static constexpr uint32_t kSize = 512 * 1024 * 1024;

  GuestMirror(const rex::ui::vulkan::VulkanDevice* device, const uint8_t* physical_base, bool host_cached);
  ~GuestMirror();
  GuestMirror(const GuestMirror&) = delete;
  GuestMirror& operator=(const GuestMirror&) = delete;

  bool valid() const { return mapping_ != nullptr; }
  VkBuffer buffer() const { return buffer_; }
  void Copy(uint32_t address, uint32_t size);

 private:
  const rex::ui::vulkan::VulkanDevice* device_;
  const uint8_t* physical_base_;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  uint32_t memory_type_ = 0;
  uint8_t* mapping_ = nullptr;
};

}  // namespace nier::native
