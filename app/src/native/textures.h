#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

#include "texture_loader.h"

namespace rex::ui::vulkan {
class VulkanDevice;
class VulkanUploadBufferPool;
}  // namespace rex::ui::vulkan

namespace nier::native {

class Textures {
 public:
  struct Binding {
    VkImageView view;
    uint16_t host_swizzle;
    uint8_t swizzled_signs;
  };
  struct Stats {
    uint64_t uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t unsupported = 0;
    uint64_t resolve_hits = 0;
  };

  Textures(const rex::ui::vulkan::VulkanDevice* device, const uint8_t* physical_base,
           rex::ui::vulkan::VulkanUploadBufferPool& staging);
  ~Textures();
  Textures(const Textures&) = delete;
  Textures& operator=(const Textures&) = delete;

  void BeginFrame();
  void Prepare(VkCommandBuffer upload);
  Binding View(VkCommandBuffer upload, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
               const rex::graphics::SpirvShader::TextureBinding& binding, uint64_t submission);
  VkSampler Sampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                    const rex::graphics::SpirvShader::SamplerBinding& binding);
  void AddResolveImage(uint32_t address, VkFormat format, uint32_t width, uint32_t height, VkImageView view);
  void Completed(uint64_t submission);
  const Stats& stats() const { return stats_; }
  size_t live_images() const { return textures_.size() + retired_.size(); }

 private:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView views[2][2] = {};
  };
  struct Texture {
    Image image;
    HostTextureFormat host;
    uint32_t width;
    uint32_t height;
    bool cube;
    uint32_t bytes;
    uint64_t data_hash;
    uint64_t checked_frame;
    uint64_t used_frame;
    uint64_t last_submission;
  };
  struct Retired {
    Image image;
    uint64_t last_submission;
  };
  struct ResolveImage {
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageView view;
  };

  Image CreateImage(const LoadedTexture& loaded);
  void Upload(VkCommandBuffer upload, const Image& image, const LoadedTexture& loaded, uint64_t submission);
  void Destroy(Image& image);
  VkImageView NullView(rex::graphics::xenos::FetchOpDimension dimension) const;
  uint64_t DataHash(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch) const;

  const rex::ui::vulkan::VulkanDevice* device_;
  const uint8_t* physical_base_;
  rex::ui::vulkan::VulkanUploadBufferPool& staging_;
  uint64_t frame_ = 1;
  bool prepared_ = false;
  Image null_array_;
  Image null_3d_;
  std::unordered_map<uint64_t, Texture> textures_;
  std::vector<Retired> retired_;
  std::unordered_map<uint32_t, ResolveImage> resolve_images_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  Stats stats_;
};

}  // namespace nier::native
