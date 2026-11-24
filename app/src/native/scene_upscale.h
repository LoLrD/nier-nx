#pragma once

#include <cstdint>
#include <unordered_map>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Submissions;

class SceneUpscale {
 public:
  SceneUpscale(const rex::ui::vulkan::VulkanDevice* device, Submissions& submissions);
  ~SceneUpscale();
  SceneUpscale(const SceneUpscale&) = delete;
  SceneUpscale& operator=(const SceneUpscale&) = delete;

  VkImageView Ready(VkImageView source, VkFormat format, uint32_t output_width, uint32_t output_height,
                    uint64_t version, float sharpness_stops) const;
  VkImageView Upscale(VkCommandBuffer command_buffer, VkImageView source, VkImageView source_plain, VkFormat format,
                      uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height, uint64_t version,
                      float sharpness_stops);
  uint64_t runs() const { return runs_; }

 private:
  struct Output {
    VkImage easu = VK_NULL_HANDLE;
    VkDeviceMemory easu_memory = VK_NULL_HANDLE;
    VkImageView easu_view = VK_NULL_HANDLE;
    VkImageView easu_array_view = VK_NULL_HANDLE;
    VkImage rcas = VK_NULL_HANDLE;
    VkDeviceMemory rcas_memory = VK_NULL_HANDLE;
    VkImageView rcas_view = VK_NULL_HANDLE;
    VkImageView rcas_array_view = VK_NULL_HANDLE;
    VkImageView source = VK_NULL_HANDLE;
    uint64_t version = 0;
    float sharpness_stops = 0.0f;
    bool initialized = false;
  };
  struct FormatPipelines {
    VkPipeline easu = VK_NULL_HANDLE;
    VkPipeline rcas = VK_NULL_HANDLE;
    VkPipeline alpha = VK_NULL_HANDLE;
  };

  Output* OutputFor(VkImageView source, VkFormat format, uint32_t width, uint32_t height);
  bool CreateImage(VkFormat format, uint32_t width, uint32_t height, VkImage& image, VkDeviceMemory& memory,
                   VkImageView& view, VkImageView& array_view);
  void DestroyOutput(Output& output);
  const FormatPipelines& PipelinesFor(VkFormat format);
  VkPipeline CreatePipeline(VkShaderModule fragment, VkFormat format, VkColorComponentFlags write_mask);
  VkDescriptorSet SourceSet(VkImageView view);
  void Initialize(VkCommandBuffer command_buffer, const Output& output);
  void Pass(VkCommandBuffer command_buffer, VkPipeline pipeline, VkDescriptorSet set, VkImageView target,
            uint32_t width, uint32_t height, const void* constants, uint32_t size, VkAttachmentLoadOp load);

  const rex::ui::vulkan::VulkanDevice* device_;
  Submissions& submissions_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkShaderModule vertex_ = VK_NULL_HANDLE;
  VkShaderModule easu_ = VK_NULL_HANDLE;
  VkShaderModule rcas_ = VK_NULL_HANDLE;
  VkShaderModule alpha_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, Output> outputs_;
  std::unordered_map<VkFormat, FormatPipelines> pipelines_;
  uint64_t runs_ = 0;
};

}  // namespace nier::native
