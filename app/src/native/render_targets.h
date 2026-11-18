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
  };
  struct Stats {
    uint64_t transfers = 0;
    uint64_t transfer_tiles = 0;
    uint64_t transfer_regions = 0;
    uint64_t incompatible_tiles = 0;
  };

  explicit RenderTargets(const rex::ui::vulkan::VulkanDevice* device);
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
  void Claim(VkCommandBuffer command_buffer, const TargetKey& key, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
  bool Begins(const TargetSet& set) const;
  void Begin(VkCommandBuffer command_buffer, const TargetSet& set);
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

  void BeginRendering(VkCommandBuffer command_buffer, const Bound& bound);
  void Discard(const TargetKey& key, const VkRect2D& rect);
  void NoteIncompatible(uint32_t owner, uint32_t foreign);

  const rex::ui::vulkan::VulkanDevice* device_;
  VkFormat depth_format_;
  std::unordered_map<TargetKey, Target, TargetKeyHash> targets_;
  bool rendering_ = false;
  VkExtent2D render_area_{};
  uint64_t bytes_ = 0;
  Bound bound_;
  EdramOwnership edram_;
  std::vector<TargetKey> owner_keys_{TargetKey{}};
  std::set<std::pair<uint32_t, uint32_t>> logged_incompatible_;
  Stats stats_;
};

void FullBarrier(const rex::ui::vulkan::VulkanDevice* device, VkCommandBuffer command_buffer);

}  // namespace nier::native
