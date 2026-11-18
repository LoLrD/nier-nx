#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>

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

  Target* Get(VkCommandBuffer command_buffer, const TargetKey& key);
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

  const rex::ui::vulkan::VulkanDevice* device_;
  VkFormat depth_format_;
  std::unordered_map<TargetKey, Target, TargetKeyHash> targets_;
  bool rendering_ = false;
  VkExtent2D render_area_{};
  uint64_t bytes_ = 0;
  Bound bound_;
};

void FullBarrier(const rex::ui::vulkan::VulkanDevice* device, VkCommandBuffer command_buffer);

}  // namespace nier::native
