#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

#include "phased_hash.h"
#include "resolve_plan.h"
#include "texture_loader.h"

namespace rex::ui::vulkan {
class VulkanDevice;
class VulkanUploadBufferPool;
}  // namespace rex::ui::vulkan

namespace nier::native {

class Textures {
 public:
  struct Binding {
    VkImageView view = VK_NULL_HANDLE;
    uint16_t host_swizzle = 0;
    uint8_t swizzled_signs = 0;
    bool out_of_memory = false;
  };
  enum class UploadReason : uint8_t { kNew, kChanged, kEvicted };
  struct UploadRecord {
    uint32_t address;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    UploadReason reason;
  };
  struct Stats {
    uint64_t uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t unsupported = 0;
    uint64_t resolve_hits = 0;
    uint64_t resolve_misses = 0;
    uint64_t reused = 0;
    uint64_t evictions = 0;
    uint64_t memory_failures = 0;
    uint64_t releases = 0;
    uint64_t loads[3] = {};
    uint64_t load_bytes[3] = {};
    uint64_t stale_reads[5] = {};
    uint64_t hashed_bytes = 0;
    uint64_t sampled_checks = 0;
    uint64_t late_changes = 0;
  };
  static constexpr size_t kTopUploads = 5;

  Textures(const rex::ui::vulkan::VulkanDevice* device, const uint8_t* physical_base,
           rex::ui::vulkan::VulkanUploadBufferPool& staging);
  ~Textures();
  Textures(const Textures&) = delete;
  Textures& operator=(const Textures&) = delete;

  void BeginFrame(uint64_t now_ms);
  void Prepare(VkCommandBuffer upload);
  Binding View(VkCommandBuffer upload, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
               const rex::graphics::SpirvShader::TextureBinding& binding, uint64_t submission);
  VkSampler Sampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                    const rex::graphics::SpirvShader::SamplerBinding& binding);
  void AddResolveImage(uint32_t address, VkFormat format, uint32_t width, uint32_t height, VkImageView view,
                       uint32_t guest_bytes);
  void RefreshResolveHash(uint32_t address);
  void RemoveResolveImage(uint32_t address, VkFormat format, VkImageView view);
  void Evict(uint64_t unused_ms);
  void ReleaseMemory();
  void Completed(uint64_t submission);
  const Stats& stats() const { return stats_; }
  std::vector<UploadRecord> TakeTopUploads();
  size_t live_images() const { return textures_.size() + retired_.size() + free_images_.size(); }
  uint64_t active_bytes() const { return active_bytes_; }
  uint64_t retired_bytes() const { return retired_bytes_; }
  uint64_t free_bytes() const { return free_bytes_; }
  uint64_t staged_bytes() const { return staged_bytes_; }
  void ResetStaged() { staged_bytes_ = 0; }

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
    PhasedHash content;
    uint64_t checked_frame;
    uint64_t used_frame;
    uint64_t used_ms;
    uint64_t last_submission;
    uint64_t shape;
  };
  struct Retired {
    Image image;
    uint64_t last_submission;
    uint32_t bytes;
    uint64_t shape;
  };
  struct FreeImage {
    Image image;
    uint32_t bytes;
  };
  struct ResolveImage {
    VkFormat format;
    uint32_t width;
    uint32_t height;
    VkImageView view;
    uint32_t guest_bytes;
    PhasedHash guest;
    uint64_t checked_frame;
    uint64_t published_frame;
  };

  Image CreateImage(const TextureShape& shape);
  Image AcquireImage(const TextureShape& shape);
  static uint64_t ShapeKey(const TextureShape& shape);
  bool Upload(VkCommandBuffer upload, const Image& image, const LoadedTexture& loaded, uint64_t submission);
  Binding OutOfMemory(uint64_t key, Binding result);
  void RetireImage(const Image& image, uint64_t submission, uint32_t bytes, uint64_t shape);
  void Destroy(Image& image);
  void Retire(Texture& texture);
  void EnforceBudget();
  void EvictOldest(uint64_t budget);
  VkImageView NullView(rex::graphics::xenos::FetchOpDimension dimension) const;
  std::array<HashSpan, 2> ContentSpans(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch) const;
  HashSpan GuestSpan(uint32_t address, uint32_t bytes) const { return {physical_base_ + address, bytes}; }
  void Reset(PhasedHash& content, std::span<const HashSpan> spans);
  bool Changed(PhasedHash& content, std::span<const HashSpan> spans);
  void RecordLoad(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, const LoadedTexture& loaded, uint32_t bytes,
                  UploadReason reason);
  void NoteResolveMiss(uint32_t address, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, VkFormat format,
                       uint32_t width, uint32_t height);
  void NoteStaleRead(StaleRead kind, uint32_t address, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                     VkFormat format, uint32_t width, uint32_t height);
  void ForgetResolveImage(uint64_t key, StaleRead reason);
  void NoteEvicted(uint64_t key);

  const rex::ui::vulkan::VulkanDevice* device_;
  const uint8_t* physical_base_;
  rex::ui::vulkan::VulkanUploadBufferPool& staging_;
  uint64_t frame_ = 1;
  uint64_t now_ms_ = 0;
  uint64_t released_frame_ = 0;
  bool prepared_ = false;
  Image null_array_;
  Image null_3d_;
  std::unordered_map<uint64_t, Texture> textures_;
  std::vector<Retired> retired_;
  std::unordered_map<uint64_t, ResolveImage> resolve_images_;
  std::unordered_map<uint32_t, uint32_t> resolve_addresses_;
  std::map<uint32_t, ResolveRecord> resolve_records_;
  uint64_t logged_stale_reads_ = 0;
  std::unordered_set<uint64_t> evicted_keys_;
  std::vector<UploadRecord> top_uploads_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  uint64_t active_bytes_ = 0;
  uint64_t retired_bytes_ = 0;
  uint64_t free_bytes_ = 0;
  std::unordered_map<uint64_t, uint64_t> failed_;
  uint64_t staged_bytes_ = 0;
  std::unordered_multimap<uint64_t, FreeImage> free_images_;
  Stats stats_;
};

}  // namespace nier::native
