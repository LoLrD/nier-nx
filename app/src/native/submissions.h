#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Submissions {
 public:
  static constexpr uint32_t kSlots = 3;
  static constexpr VkDeviceSize kUniformBytes = 8 << 20;
  static constexpr uint32_t kFloatConstantsBytes = 4096;

  Submissions(const rex::ui::vulkan::VulkanDevice* device, VkDescriptorSetLayout constants_layout);
  ~Submissions();
  Submissions(const Submissions&) = delete;
  Submissions& operator=(const Submissions&) = delete;

  bool open() const { return open_; }
  uint64_t current() const { return current_; }
  uint64_t completed() const { return completed_; }
  VkCommandBuffer upload() const { return slots_[current_ % kSlots].upload; }
  VkCommandBuffer main() const { return slots_[current_ % kSlots].main; }
  VkDescriptorSet constants_set() const { return slots_[current_ % kSlots].constants_set; }
  VkBuffer uniform_buffer() const { return slots_[current_ % kSlots].uniforms; }

  void Begin();
  bool HasUniformSpace(VkDeviceSize bytes) const;
  bool Uniform(const void* data, size_t size, size_t reserve, uint32_t& offset);
  VkDescriptorSet Allocate(VkDescriptorSetLayout layout);
  void Submit();
  void Wait(uint64_t submission);
  bool Poll();
  void WaitIdle();

 private:
  struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE;
    VkCommandBuffer main = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkBuffer uniforms = VK_NULL_HANDLE;
    VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
    uint32_t uniform_memory_type = 0;
    uint8_t* uniform_mapping = nullptr;
    VkDeviceSize uniform_used = 0;
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
    uint64_t submission = 0;
  };

  const rex::ui::vulkan::VulkanDevice* device_;
  std::array<Slot, kSlots> slots_;
  VkDescriptorPool constants_pool_ = VK_NULL_HANDLE;
  uint64_t current_ = 1;
  uint64_t completed_ = 0;
  bool open_ = false;
};

}  // namespace nier::native
