#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace nier::native {

struct HostTextureFormat {
  enum class Convert { kNone, kCtx1ToR8G8, kDepthUnormToFloat };

  VkFormat format;
  VkFormat signed_format;
  uint32_t block_width;
  uint32_t block_height;
  uint32_t bytes_per_block;
  uint32_t swizzle;
  Convert convert;
};

struct LoadedTexture {
  HostTextureFormat host;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t mips;
  bool cube;
  std::vector<std::vector<uint8_t>> levels;
};

struct TextureFootprint {
  uint32_t base_address;
  uint32_t base_size;
  uint32_t mip_address;
  uint32_t mip_size;
};

std::optional<HostTextureFormat> HostFormatFor(rex::graphics::xenos::TextureFormat format);
TextureFootprint FootprintOf(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
std::optional<LoadedTexture> LoadTexture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                                         const uint8_t* physical_base);
bool NeedsRehash(uint32_t bytes, uint64_t frame, uint64_t last_checked_frame, bool used_this_frame);
bool ResolveImageMatches(VkFormat image_format, uint32_t image_width, uint32_t image_height, VkFormat wanted_format,
                         uint32_t width, uint32_t height);

}  // namespace nier::native
