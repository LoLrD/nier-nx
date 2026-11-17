#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "render_targets.h"
#include "resolve_plan.h"
#include "textures.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Resolves {
 public:
  struct Stats {
    uint64_t copies = 0;
    uint64_t depth_copies = 0;
    uint64_t clears = 0;
    uint64_t readbacks = 0;
    uint64_t skipped = 0;
  };

  Resolves(const rex::ui::vulkan::VulkanDevice* device, RenderTargets& targets, Textures& textures,
           uint8_t* physical_base);
  ~Resolves();
  Resolves(const Resolves&) = delete;
  Resolves& operator=(const Resolves&) = delete;

  void Resolve(VkCommandBuffer command_buffer, const ResolvePlan& plan, uint64_t submission);
  void Completed(uint64_t submission);
  VkImage FindImage(uint32_t address, uint32_t& width, uint32_t& height) const;
  const Stats& stats() const { return stats_; }

 private:
  struct DestImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView storage_view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  struct ReadbackBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
  };
  struct PendingReadback {
    uint64_t submission;
    ReadbackBuffer buffer;
    ResolvePlan plan;
  };

  DestImage& Dest(VkCommandBuffer command_buffer, const ResolvePlan& plan, VkFormat format);
  void CopyColor(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                 const DestImage& dest);
  void CopyDepth(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                 const DestImage& dest);
  void Readback(VkCommandBuffer command_buffer, const ResolvePlan& plan, const DestImage& dest, uint64_t submission);
  ReadbackBuffer AcquireReadbackBuffer();
  VkDescriptorSet DepthDescriptorSet(VkImageView source, VkImageView dest);

  const rex::ui::vulkan::VulkanDevice* device_;
  RenderTargets& targets_;
  Textures& textures_;
  uint8_t* physical_base_;
  std::unordered_map<uint64_t, DestImage> images_;
  std::unordered_map<uint32_t, uint64_t> latest_by_address_;
  std::vector<ReadbackBuffer> free_readbacks_;
  std::vector<PendingReadback> pending_;
  VkDescriptorSetLayout depth_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool depth_pool_ = VK_NULL_HANDLE;
  VkSampler depth_sampler_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkDescriptorSet> depth_sets_;
  Stats stats_;
};

}  // namespace nier::native
