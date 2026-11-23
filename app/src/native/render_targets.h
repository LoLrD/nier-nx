#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "target_keys.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Submissions;

class RenderTargets {
 public:
  struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView attachment_view = VK_NULL_HANDLE;
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    TargetExtent extent{};
    VkDeviceSize bytes = 0;
    uint32_t owner = 0;
    uint64_t rendered = 0;
    uint32_t shift = 0;
    uint64_t writes = 0;
  };
  struct Stats {
    uint64_t transfers = 0;
    uint64_t transfer_tiles = 0;
    uint64_t transfer_regions = 0;
    uint64_t incompatible_tiles = 0;
    uint64_t depth_tiles = 0;
  };

  RenderTargets(const rex::ui::vulkan::VulkanDevice* device, Submissions& submissions);
  ~RenderTargets();
  RenderTargets(const RenderTargets&) = delete;
  RenderTargets& operator=(const RenderTargets&) = delete;

  static bool Supported(const TargetKey& key);
  VkFormat depth_format() const { return depth_format_; }
  size_t count() const { return targets_.size(); }
  uint64_t bytes() const { return bytes_; }
  bool rendering() const { return rendering_; }
  VkExtent2D render_area() const { return render_area_; }
  const Stats& stats() const { return stats_; }

  Target* Get(VkCommandBuffer command_buffer, const TargetKey& key);
  void Claim(VkCommandBuffer command_buffer, const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
             bool newer_than_rendering);
  bool Begins(const TargetSet& set) const;
  void Begin(VkCommandBuffer command_buffer, const TargetSet& set, uint32_t rows, uint32_t shift);
  void End(VkCommandBuffer command_buffer);
  void ClearColor(VkCommandBuffer command_buffer, const TargetKey& key, const VkRect2D& rect, const float color[4]);
  void ClearDepth(VkCommandBuffer command_buffer, const TargetKey& key, const VkRect2D& rect, float depth,
                  uint32_t stencil);

 private:
  struct Bound {
    std::array<std::optional<TargetKey>, 4> color;
    std::optional<TargetKey> depth;
    bool operator==(const Bound&) const = default;
  };

  void CreateConverter();
  void Own(VkCommandBuffer command_buffer, const Bound& bound, uint32_t from_rows, uint32_t to_rows, uint32_t shift);
  void BeginRendering(VkCommandBuffer command_buffer, const Bound& bound);
  void ConvertDepth(VkCommandBuffer command_buffer, const Target& depth, const Target& color,
                    const std::vector<VkImageCopy>& regions);
  VkDescriptorSet ConvertSet(const Target& depth, const Target& color);
  void Discard(const TargetKey& key, const VkRect2D& rect);
  void NoteIncompatible(uint32_t owner, uint32_t foreign);

  const rex::ui::vulkan::VulkanDevice* device_;
  Submissions& submissions_;
  VkFormat depth_format_;
  std::unordered_map<TargetKey, Target, TargetKeyHash> targets_;
  bool rendering_ = false;
  VkExtent2D render_area_{};
  uint64_t bytes_ = 0;
  Bound bound_;
  uint32_t claimed_rows_ = 0;
  VkDescriptorSetLayout convert_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout convert_layout_ = VK_NULL_HANDLE;
  VkPipeline convert_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool convert_pool_ = VK_NULL_HANDLE;
  VkSampler convert_sampler_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkDescriptorSet> convert_sets_;
  EdramOwnership edram_;
  std::vector<TargetKey> owner_keys_{TargetKey{}};
  std::set<std::pair<uint32_t, uint32_t>> logged_incompatible_;
  Stats stats_;
  uint64_t sequence_ = 0;
};

void FullBarrier(const rex::ui::vulkan::VulkanDevice* device, VkCommandBuffer command_buffer);

}  // namespace nier::native
