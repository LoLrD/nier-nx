#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace nier::native {

struct DynamicState {
  VkViewport viewport{};
  VkRect2D scissor{};
  float depth_bias_constant = 0.0f;
  float depth_bias_slope = 0.0f;
  std::array<float, 4> blend{};
  std::array<uint32_t, 2> compare_mask{};
  std::array<uint32_t, 2> write_mask{};
  std::array<uint32_t, 2> reference{};
};

enum DynamicStateBit : uint32_t {
  kViewportBit = 1u << 0,
  kScissorBit = 1u << 1,
  kDepthBiasBit = 1u << 2,
  kBlendBit = 1u << 3,
  kCompareMaskFrontBit = 1u << 4,
  kCompareMaskBackBit = 1u << 5,
  kWriteMaskFrontBit = 1u << 6,
  kWriteMaskBackBit = 1u << 7,
  kReferenceFrontBit = 1u << 8,
  kReferenceBackBit = 1u << 9,
  kAllDynamicState = (1u << 10) - 1,
};

class DynamicStateFilter {
 public:
  uint32_t Changed(const DynamicState& next);
  void Reset() { valid_ = false; }

 private:
  DynamicState last_{};
  bool valid_ = false;
};

inline constexpr uint32_t kBoundSets = 4;
inline constexpr uint32_t kConstantsSet = 1;
inline constexpr uint32_t kDynamicOffsets = 5;

struct DescriptorBinding {
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kBoundSets> sets{};
  std::array<uint32_t, kDynamicOffsets> offsets{};
};

class DescriptorBindingFilter {
 public:
  uint32_t FirstChanged(const DescriptorBinding& next);
  void Reset() { valid_ = false; }

 private:
  DescriptorBinding last_{};
  bool valid_ = false;
};

class RepeatedUniforms {
 public:
  bool Find(uint32_t stream, const void* data, size_t size, uint32_t& offset) const;
  void Remember(uint32_t stream, const void* data, size_t size, uint32_t offset);
  void Clear();

 private:
  struct Entry {
    std::vector<uint8_t> bytes;
    uint32_t offset = 0;
    bool valid = false;
  };
  std::array<Entry, kDynamicOffsets> entries_;
};

}  // namespace nier::native
