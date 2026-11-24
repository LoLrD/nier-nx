#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "draw_state_filter.h"
#include "gpu_times.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Submissions {
 public:
  static constexpr uint32_t kSlots = 3;
  static constexpr VkDeviceSize kUniformBytes = 8 << 20;
  static constexpr uint32_t kFloatConstantsBytes = 4096;
  static constexpr uint32_t kTimestamps = 1024;

  Submissions(const rex::ui::vulkan::VulkanDevice* device, VkDescriptorSetLayout constants_layout, bool host_cached);
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
  const std::array<uint64_t, kGpuCategories>& gpu_ns() const { return gpu_ns_; }
  const std::vector<uint64_t>& pass_ns() const { return pass_ns_; }

  void Begin();
  bool HasUniformSpace(VkDeviceSize bytes) const;
  bool Uniform(const void* data, size_t size, size_t reserve, uint32_t& offset);
  bool RepeatedUniform(uint32_t stream, const void* data, size_t size, size_t reserve, uint32_t& offset,
                       bool& reused);
  VkDescriptorSet Allocate(VkDescriptorSetLayout layout);
  VkDescriptorSet AllocateShared(VkDescriptorSetLayout layout, uint64_t key, bool& fresh);
  void Mark(GpuCategory category, uint16_t pass = 0);
  void Interrupt(GpuCategory category);
  void Resume();
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
    VkQueryPool queries = VK_NULL_HANDLE;
    std::vector<GpuCategory> marks;
    std::vector<uint16_t> passes;
    std::unordered_map<uint64_t, VkDescriptorSet> shared_sets;
  };

  void SetupTimestamps();
  void CollectTimes(Slot& slot);
  void WriteTimestamp(Slot& slot, VkCommandBuffer command_buffer, uint32_t index, GpuCategory category, uint16_t pass);

  const rex::ui::vulkan::VulkanDevice* device_;
  std::array<Slot, kSlots> slots_;
  VkDescriptorPool constants_pool_ = VK_NULL_HANDLE;
  uint64_t current_ = 1;
  uint64_t completed_ = 0;
  bool open_ = false;
  double timestamp_period_ns_ = 0.0;
  GpuCategory resume_category_ = GpuCategory::kNone;
  uint16_t resume_pass_ = 0;
  uint64_t timestamp_mask_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_ns_{};
  std::vector<uint64_t> pass_ns_;
  RepeatedUniforms repeated_;
};

}  // namespace nier::native
