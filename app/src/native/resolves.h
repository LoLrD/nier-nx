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
    uint64_t stale_readbacks = 0;
    uint64_t skipped = 0;
  };

  Resolves(const rex::ui::vulkan::VulkanDevice* device, RenderTargets& targets, Textures& textures,
           uint8_t* physical_base);
  ~Resolves();
  Resolves(const Resolves&) = delete;
  Resolves& operator=(const Resolves&) = delete;

  void Resolve(VkCommandBuffer command_buffer, const ResolvePlan& plan, uint64_t submission);
  void BeginFrame() { ++frame_; }
  void Evict(uint64_t unused_frames, uint64_t submission);
  void Completed(uint64_t submission);
  size_t image_count() const { return images_.size(); }
  uint64_t bytes() const { return bytes_; }
  VkImage FindImage(uint32_t address, uint32_t& width, uint32_t& height, VkImageView* view = nullptr,
                    VkFormat* format = nullptr) const;
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
    uint32_t address = 0;
    uint64_t last_frame = 0;
    uint64_t last_submission = 0;
    VkDeviceSize bytes = 0;
    uint64_t key = 0;
  };
  struct RetiredImage {
    DestImage image;
    uint64_t submission;
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
    PendingRange range;
  };
  struct RetiredSet {
    VkDescriptorSet set;
    uint64_t submission;
  };

  DestImage* Dest(VkCommandBuffer command_buffer, const ResolvePlan& plan, VkFormat format);
  void CopyColor(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                 const DestImage& dest);
  void CopyDepth(VkCommandBuffer command_buffer, const ResolvePlan& plan, const RenderTargets::Target& source,
                 const DestImage& dest, const DestImage& raw);
  void Readback(VkCommandBuffer command_buffer, const ResolvePlan& plan, const DestImage& dest, uint64_t submission);
  ReadbackBuffer AcquireReadbackBuffer();
  VkDescriptorSet DepthDescriptorSet(VkImageView source, VkImageView dest, VkImageView raw);
  void Publish(const ResolvePlan& plan, DestImage& dest, uint32_t bytes_per_pixel, uint64_t submission);
  void Destroy(DestImage& image);

  const rex::ui::vulkan::VulkanDevice* device_;
  RenderTargets& targets_;
  Textures& textures_;
  uint8_t* physical_base_;
  std::unordered_map<uint64_t, DestImage> images_;
  std::unordered_map<uint32_t, uint64_t> presentable_by_address_;
  std::vector<ReadbackBuffer> free_readbacks_;
  std::vector<PendingReadback> pending_;
  VkDescriptorSetLayout depth_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_layout_ = VK_NULL_HANDLE;
  VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool depth_pool_ = VK_NULL_HANDLE;
  VkSampler depth_sampler_ = VK_NULL_HANDLE;
  struct DepthSet {
    VkImageView dest;
    VkImageView raw;
    VkDescriptorSet set;
  };
  std::unordered_map<uint64_t, DepthSet> depth_sets_;
  std::vector<RetiredImage> retired_;
  std::vector<RetiredSet> retired_sets_;
  uint64_t frame_ = 0;
  uint64_t bytes_ = 0;
  Stats stats_;
};

}  // namespace nier::native
