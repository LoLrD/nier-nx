#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "pipeline_state.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Pipelines {
 public:
  explicit Pipelines(const rex::ui::vulkan::VulkanDevice* device);
  ~Pipelines();
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;

  VkDescriptorSetLayout SharedMemoryLayout() const { return shared_memory_layout_; }
  VkDescriptorSetLayout ConstantsLayout() const { return constants_layout_; }
  uint32_t shared_memory_binding_count() const { return shared_memory_binding_count_; }
  size_t pipeline_count() const { return pipeline_count_; }

  VkDescriptorSetLayout TextureSetLayout(bool pixel, uint32_t textures, uint32_t samplers);
  VkPipelineLayout Layout(uint32_t vs_textures, uint32_t vs_samplers, uint32_t ps_textures, uint32_t ps_samplers);
  VkPipeline Get(const PipelineState& state, VkPipelineLayout layout);

 private:
  struct Entry {
    PipelineState state;
    VkPipelineLayout layout;
    VkPipeline pipeline;
  };

  VkPipeline Create(const PipelineState& state, VkPipelineLayout layout) const;

  const rex::ui::vulkan::VulkanDevice* device_;
  uint32_t shared_memory_binding_count_;
  VkDescriptorSetLayout shared_memory_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_layouts_;
  std::unordered_map<uint64_t, VkPipelineLayout> layouts_;
  std::unordered_map<uint64_t, std::vector<Entry>> pipelines_;
  size_t pipeline_count_ = 0;
  VkPipelineCreateFlags create_flags_;
};

}  // namespace nier::native
