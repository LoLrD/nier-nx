#include "textures.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>
#include <rex/ui/vulkan/util.h>

#include "vk_recorder.h"

REXCVAR_DEFINE_INT32(nier_texture_budget_mb, 448, "NieR",
                     "Native renderer: texture memory the cache keeps before evicting textures unused for a second");

namespace nier::native {

using namespace rex::graphics;
namespace vk = rex::ui::vulkan;

namespace {

constexpr uint64_t kFreeImageBytes = 48ull << 20;
constexpr uint64_t kMinAgeMs = 1000;
constexpr uint64_t kFailedRetryMs = 2000;
constexpr uint64_t kLoggedStaleReads = 40;

constexpr VkSamplerAddressMode kAddressModes[8] = {
    VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};

uint64_t ResolveKey(uint32_t address, VkFormat format) { return uint64_t(address) << 32 | uint32_t(format); }

uint64_t TextureKey(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const std::array<uint32_t, 11> fields = {
      fetch.base_address, fetch.mip_address,   uint32_t(fetch.format), uint32_t(fetch.endianness),
      uint32_t(fetch.dimension), fetch.dword_2, fetch.pitch,           fetch.tiled,
      fetch.packed_mips,   fetch.mip_max_level, fetch.stacked};
  return XXH3_64bits(fields.data(), sizeof(fields));
}

VkImageViewType ViewType(xenos::FetchOpDimension dimension) {
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      return VK_IMAGE_VIEW_TYPE_3D;
    case xenos::FetchOpDimension::kCube:
      return VK_IMAGE_VIEW_TYPE_CUBE;
    default:
      return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  }
}

}  // namespace

Textures::Textures(const vk::VulkanDevice* device, const uint8_t* physical_base, vk::VulkanUploadBufferPool& staging)
    : device_(device), physical_base_(physical_base), staging_(staging) {
  const auto& dfn = Vk(device_);
  const VkDevice d = device_->device();
  auto make = [&](VkImageType type, uint32_t layers, VkImageCreateFlags flags) {
    Image image;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = type;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, image.image,
                                             image.memory);
    return image;
  };
  null_array_ = make(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
  null_3d_ = make(VK_IMAGE_TYPE_3D, 1, 0);
  auto view = [&](VkImage image, VkImageViewType type, uint32_t layers) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = type;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageView result = VK_NULL_HANDLE;
    dfn.vkCreateImageView(d, &info, nullptr, &result);
    return result;
  };
  null_array_.views[0][0] = view(null_array_.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
  null_array_.views[0][1] = view(null_array_.image, VK_IMAGE_VIEW_TYPE_CUBE, 6);
  null_3d_.views[0][0] = view(null_3d_.image, VK_IMAGE_VIEW_TYPE_3D, 1);
}

Textures::~Textures() {
  for (auto& [key, texture] : textures_) {
    Destroy(texture.image);
  }
  for (Retired& retired : retired_) {
    Destroy(retired.image);
  }
  for (auto& [shape, free] : free_images_) {
    Destroy(free.image);
  }
  for (auto& [key, sampler] : samplers_) {
    Vk(device_).vkDestroySampler(device_->device(), sampler, nullptr);
  }
  Destroy(null_array_);
  Destroy(null_3d_);
}

void Textures::BeginFrame(uint64_t now_ms) {
  ++frame_;
  now_ms_ = now_ms;
  if (cards_) {
    cards_->NextInterval();
  }
}

void Textures::SetCards(uint8_t* table, bool enabled, bool verify) {
  if (enabled && !cards_) {
    cards_ = std::make_unique<StoreCards>(table, kTextureCards);
  }
  cards_enabled_ = enabled;
  cards_verify_ = enabled && verify;
}

uint64_t Textures::TakeCards(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const TextureFootprint footprint = FootprintOf(fetch);
  card_runs_.clear();
  cards_->Take(footprint.base_address, footprint.base_size, card_runs_);
  cards_->Take(footprint.mip_address, footprint.mip_size, card_runs_);
  ++card_serial_;
  card_serials_.Mark(card_runs_, card_serial_);
  return std::max(card_serials_.Newest(footprint.base_address, footprint.base_size),
                  card_serials_.Newest(footprint.mip_address, footprint.mip_size));
}

void Textures::Prepare(VkCommandBuffer upload) {
  if (prepared_) {
    return;
  }
  prepared_ = true;
  const auto& dfn = Vk(device_);
  std::array<VkImageMemoryBarrier, 2> barriers{};
  const std::array<VkImage, 2> images = {null_array_.image, null_3d_.image};
  for (size_t i = 0; i < barriers.size(); ++i) {
    barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = images[i];
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
  }
  dfn.vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                           0, nullptr, uint32_t(barriers.size()), barriers.data());
  const VkClearColorValue black{};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
  for (VkImage image : images) {
    dfn.vkCmdClearColorImage(upload, image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
  }
}

std::array<HashSpan, 2> Textures::ContentSpans(const xenos::xe_gpu_texture_fetch_t& fetch) const {
  const TextureFootprint footprint = FootprintOf(fetch);
  return {HashSpan{physical_base_ + footprint.base_address, footprint.base_size},
          HashSpan{physical_base_ + footprint.mip_address, footprint.mip_size}};
}

void Textures::Reset(PhasedHash& content, std::span<const HashSpan> spans) {
  stats_.hashed_bytes += content.Reset(spans);
}

bool Textures::Changed(PhasedHash& content, std::span<const HashSpan> spans) {
  const PhasedHash::Result result = content.Check(spans);
  stats_.hashed_bytes += result.hashed_bytes;
  stats_.sampled_checks += result.sampled;
  stats_.sampled_changes += result.changed && result.sampled;
  stats_.possibly_late += result.possibly_late;
  return result.changed;
}

Textures::Binding Textures::View(VkCommandBuffer upload, const xenos::xe_gpu_texture_fetch_t& fetch,
                                 const SpirvShader::TextureBinding& binding, uint64_t submission) {
  const auto host = HostFormatFor(fetch.format);
  Binding result{NullView(binding.dimension), 0, texture_util::SwizzleSigns(fetch)};
  if (!host) {
    ++stats_.unsupported;
    return result;
  }
  result.host_swizzle = uint16_t(TextureCache::GuestToHostSwizzle(fetch.swizzle, host->swizzle));
  const bool is_signed = binding.is_signed && host->signed_format != host->format;
  const VkFormat wanted = is_signed ? host->signed_format : host->format;
  const uint32_t width = fetch.size_2d.width + 1;
  const uint32_t height = fetch.size_2d.height + 1;
  if (binding.dimension != xenos::FetchOpDimension::kCube &&
      binding.dimension != xenos::FetchOpDimension::k3DOrStacked) {
    const uint32_t address = fetch.base_address << 12;
    if (auto it = resolve_images_.find(ResolveKey(address, host->format)); it != resolve_images_.end()) {
      ResolveImage& resolved = it->second;
      if (NeedsRehash(resolved.guest_bytes, frame_, resolved.checked_frame, resolved.checked_frame == frame_, false)) {
        resolved.checked_frame = frame_;
        const HashSpan span = GuestSpan(address, resolved.guest_bytes);
        if (Changed(resolved.guest, {&span, 1})) {
          ForgetResolveImage(it->first, StaleRead::kForgotten);
          it = resolve_images_.end();
        }
      }
      if (it != resolve_images_.end() &&
          ResolveImageMatches(resolved.format, resolved.width, resolved.height, host->format, width, height)) {
        ++stats_.resolve_hits;
        result.resolve_address = address;
        if (OlderThanNewest(resolve_records_, address, resolved.published_frame)) {
          NoteStaleRead(StaleRead::kOlderImage, address, fetch, host->format, width, height);
        }
        result.view = resolved.view;
        return result;
      }
    }
    if (resolve_addresses_.contains(address)) {
      NoteResolveMiss(address, fetch, host->format, width, height);
    } else if (const StaleRead kind = GuestReadKind(resolve_records_, address, frame_); kind != StaleRead::kNone) {
      NoteStaleRead(kind, address, fetch, host->format, width, height);
    }
  }
  if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) {
    ++stats_.unsupported;
    return result;
  }
  const uint64_t key = TextureKey(fetch);
  auto it = textures_.find(key);
  bool reload = it == textures_.end();
  PhasedHash content;
  UploadReason reason = UploadReason::kNew;
  if (!reload && cards_enabled_) {
    Texture& texture = it->second;
    if (texture.checked_frame != frame_) {
      texture.checked_frame = frame_;
      const bool marked = TakeCards(fetch) > texture.cards_serial;
      texture.cards_serial = card_serial_;
      bool changed = false;
      if (marked || cards_verify_) {
        const auto spans = ContentSpans(fetch);
        for (const HashSpan& span : spans) {
          stats_.hashed_bytes += span.bytes;
        }
        changed = !texture.content.Matches(spans);
        stats_.cards_missed += changed && !marked;
        stats_.cards_unchanged += marked && !changed;
      }
      if (changed) {
        ++stats_.cards_reloads;
        Retire(texture);
        textures_.erase(it);
        reload = true;
        reason = UploadReason::kChanged;
      }
    }
  } else if (!reload) {
    Texture& texture = it->second;
    if (NeedsRehash(texture.bytes, frame_, texture.checked_frame, texture.used_frame == frame_,
                    texture.content.sampling())) {
      texture.checked_frame = frame_;
      if (Changed(texture.content, ContentSpans(fetch))) {
        content = texture.content;
        Retire(texture);
        textures_.erase(it);
        reload = true;
        reason = UploadReason::kChanged;
      }
    }
  } else if (evicted_keys_.erase(key)) {
    reason = UploadReason::kEvicted;
  }
  if (reload) {
    if (auto failed = failed_.find(key); failed != failed_.end()) {
      if (now_ms_ < failed->second + kFailedRetryMs) {
        ++stats_.memory_failures;
        return result;
      }
      failed_.erase(failed);
    }
    const auto shape = ShapeFor(fetch);
    if (!shape || shape->level_bytes > staging_.page_size()) {
      ++stats_.unsupported;
      return result;
    }
    if (cards_enabled_ || reason != UploadReason::kChanged) {
      Reset(content, ContentSpans(fetch));
    }
    uint64_t cards_serial = 0;
    if (cards_enabled_) {
      TakeCards(fetch);
      cards_serial = card_serial_;
    }
    const Image image = AcquireImage(*shape);
    if (image.image == VK_NULL_HANDLE) {
      return OutOfMemory(key, result);
    }
    const auto loaded = LoadTexture(fetch, physical_base_);
    if (!Upload(upload, image, *loaded, submission)) {
      RetireImage(image, submission, shape->bytes, ShapeKey(*shape));
      return OutOfMemory(key, result);
    }
    RecordLoad(fetch, *loaded, shape->bytes, reason);
    active_bytes_ += shape->bytes;
    textures_.emplace(key, Texture{image, shape->host, shape->width, shape->height, shape->cube, shape->bytes, content,
                                   frame_, frame_, now_ms_, submission, ShapeKey(*shape), cards_serial});
    EnforceBudget();
    it = textures_.find(key);
  }
  Texture& texture = it->second;
  texture.used_frame = frame_;
  texture.used_ms = now_ms_;
  texture.last_submission = submission;
  const bool cube_view = binding.dimension == xenos::FetchOpDimension::kCube;
  if (cube_view && !texture.cube) {
    ++stats_.unsupported;
    return result;
  }
  result.view = texture.image.views[is_signed][cube_view];
  return result;
}

Textures::Binding Textures::OutOfMemory(uint64_t key, Binding result) {
  if (released_frame_ == frame_) {
    failed_[key] = now_ms_;
    ++stats_.memory_failures;
    return result;
  }
  result.out_of_memory = true;
  return result;
}

Textures::Image Textures::CreateImage(const TextureShape& loaded) {
  Image image;
  const auto& dfn = Vk(device_);
  const bool mutable_format = loaded.host.signed_format != loaded.host.format;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = (loaded.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
               (mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = loaded.host.format;
  info.extent = {loaded.width, loaded.height, 1};
  info.mipLevels = loaded.mips;
  info.arrayLayers = loaded.layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!vk::util::CreateDedicatedAllocationImage(device_, info, vk::util::MemoryPurpose::kDeviceLocal, image.image,
                                                image.memory)) {
    REXLOG_ERROR("[native] could not create a {}x{} texture", loaded.width, loaded.height);
    return {};
  }
  for (uint32_t is_signed = 0; is_signed < (mutable_format ? 2u : 1u); ++is_signed) {
    for (uint32_t cube = 0; cube < (loaded.cube ? 2u : 1u); ++cube) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = image.image;
      view.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      view.format = is_signed ? loaded.host.signed_format : loaded.host.format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, loaded.mips, 0, loaded.layers};
      dfn.vkCreateImageView(device_->device(), &view, nullptr, &image.views[is_signed][cube]);
    }
  }
  if (!mutable_format) {
    image.views[1][0] = image.views[0][0];
    image.views[1][1] = image.views[0][1];
  }
  return image;
}

bool Textures::Upload(VkCommandBuffer upload, const Image& image, const LoadedTexture& loaded, uint64_t submission) {
  const auto& dfn = Vk(device_);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, loaded.mips, 0, loaded.layers};
  dfn.vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
  for (uint32_t level = 0; level < loaded.mips; ++level) {
    const auto& data = loaded.levels[level];
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = staging_.Request(submission, data.size(), 16, buffer, offset);
    if (!mapping) {
      REXLOG_ERROR("[native] no staging space for a {}-byte texture level", data.size());
      return false;
    }
    std::memcpy(mapping, data.data(), data.size());
    staged_bytes_ += data.size();
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, loaded.layers};
    region.imageExtent = {std::max(loaded.width >> level, 1u), std::max(loaded.height >> level, 1u), 1};
    dfn.vkCmdCopyBufferToImage(upload, buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    ++stats_.uploads;
    stats_.upload_bytes += data.size();
  }
  return true;
}

void Textures::Destroy(Image& image) {
  const auto& dfn = Vk(device_);
  const VkDevice d = device_->device();
  VkImageView destroyed[4] = {};
  size_t count = 0;
  for (auto& row : image.views) {
    for (VkImageView view : row) {
      if (view != VK_NULL_HANDLE && std::find(destroyed, destroyed + count, view) == destroyed + count) {
        destroyed[count++] = view;
        dfn.vkDestroyImageView(d, view, nullptr);
      }
    }
  }
  dfn.vkDestroyImage(d, image.image, nullptr);
  dfn.vkFreeMemory(d, image.memory, nullptr);
  image = {};
}

VkImageView Textures::NullView(xenos::FetchOpDimension dimension) const {
  switch (ViewType(dimension)) {
    case VK_IMAGE_VIEW_TYPE_3D:
      return null_3d_.views[0][0];
    case VK_IMAGE_VIEW_TYPE_CUBE:
      return null_array_.views[0][1];
    default:
      return null_array_.views[0][0];
  }
}

VkSampler Textures::Sampler(const xenos::xe_gpu_texture_fetch_t& fetch, const SpirvShader::SamplerBinding& binding) {
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  auto pick = [](xenos::TextureFilter bound, xenos::TextureFilter fetched) {
    return bound == xenos::TextureFilter::kUseFetchConst ? fetched : bound;
  };
  const xenos::TextureFilter mip = pick(binding.mip_filter, fetch.mip_filter);
  bool mag_linear = pick(binding.mag_filter, fetch.mag_filter) == xenos::TextureFilter::kLinear;
  bool min_linear = pick(binding.min_filter, fetch.min_filter) == xenos::TextureFilter::kLinear;
  bool mip_linear = mip == xenos::TextureFilter::kLinear;
  const bool base_map = mip == xenos::TextureFilter::kBaseMap;
  xenos::AnisoFilter aniso =
      binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter : binding.aniso_filter;
  if (aniso > xenos::AnisoFilter::kMax_16_1) {
    aniso = xenos::AnisoFilter::kDisabled;
  }
  if (aniso != xenos::AnisoFilter::kDisabled) {
    mag_linear = min_linear = mip_linear = true;
  }
  const bool border = xenos::ClampModeUsesBorder(clamp_x) || xenos::ClampModeUsesBorder(clamp_y) ||
                      xenos::ClampModeUsesBorder(clamp_z);
  const bool white = border && fetch.border_color == xenos::BorderColor::k_ABGR_White;
  const uint64_t key = uint64_t(clamp_x) | uint64_t(clamp_y) << 3 | uint64_t(clamp_z) << 6 |
                       uint64_t(mag_linear) << 9 | uint64_t(min_linear) << 10 | uint64_t(mip_linear) << 11 |
                       uint64_t(base_map) << 12 | uint64_t(aniso) << 13 | uint64_t(fetch.mip_min_level) << 16 |
                       uint64_t(white) << 20;
  auto [it, inserted] = samplers_.try_emplace(key, VK_NULL_HANDLE);
  if (!inserted) {
    return it->second;
  }
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = kAddressModes[uint32_t(clamp_x) & 7];
  info.addressModeV = kAddressModes[uint32_t(clamp_y) & 7];
  info.addressModeW = kAddressModes[uint32_t(clamp_z) & 7];
  if (aniso != xenos::AnisoFilter::kDisabled) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy =
        std::min(float(1u << (uint32_t(aniso) - 1)), device_->properties().maxSamplerAnisotropy);
  }
  info.minLod = float(fetch.mip_min_level);
  info.maxLod = base_map ? info.minLod + 0.25f : VK_LOD_CLAMP_NONE;
  info.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  Vk(device_).vkCreateSampler(device_->device(), &info, nullptr, &it->second);
  return it->second;
}

void Textures::AddResolveImage(uint32_t address, VkFormat format, uint32_t width, uint32_t height, VkImageView view,
                               uint32_t guest_bytes) {
  resolve_records_[address] = {address, guest_bytes, frame_, uint32_t(format), StaleRead::kNone, width, height};
  auto [it, inserted] = resolve_images_.try_emplace(ResolveKey(address, format));
  if (inserted) {
    ++resolve_addresses_[address];
  }
  ResolveImage& resolved = it->second;
  resolved.published_frame = frame_;
  if (!inserted && resolved.view == view && resolved.guest_bytes == guest_bytes) {
    return;
  }
  resolved = {format, width, height, view, guest_bytes, PhasedHash{}, frame_, frame_};
  const HashSpan span = GuestSpan(address, guest_bytes);
  Reset(resolved.guest, {&span, 1});
}

void Textures::RefreshResolveHash(uint32_t address) {
  for (auto& [key, resolved] : resolve_images_) {
    if (uint32_t(key >> 32) == address) {
      const HashSpan span = GuestSpan(address, resolved.guest_bytes);
      Reset(resolved.guest, {&span, 1});
      resolved.checked_frame = frame_;
    }
  }
}

void Textures::RemoveResolveImage(uint32_t address, VkFormat format, VkImageView view) {
  if (auto it = resolve_images_.find(ResolveKey(address, format));
      it != resolve_images_.end() && it->second.view == view) {
    ForgetResolveImage(it->first, StaleRead::kRemoved);
  }
}

void Textures::ForgetResolveImage(uint64_t key, StaleRead reason) {
  const uint32_t address = uint32_t(key >> 32);
  if (auto count = resolve_addresses_.find(address); count != resolve_addresses_.end() && !--count->second) {
    resolve_addresses_.erase(count);
    if (auto record = resolve_records_.find(address); record != resolve_records_.end()) {
      record->second.lost = reason;
    }
  }
  resolve_images_.erase(key);
}

void Textures::NoteStaleRead(StaleRead kind, uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch,
                             VkFormat format, uint32_t width, uint32_t height) {
  ++stats_.stale_reads[size_t(kind)];
  if (++logged_stale_reads_ > kLoggedStaleReads) {
    return;
  }
  constexpr const char* kKinds[] = {"none", "older image", "forgotten by hash", "removed", "inside a resolved range"};
  auto it = resolve_records_.upper_bound(address);
  const ResolveRecord& last = std::prev(it)->second;
  REXLOG_INFO("[native] stale read ({}): texture {:08X} {}x{} guest format {} host {}; last resolve at {:08X} {}x{} "
              "host {} {} KB, {} frames ago",
              kKinds[size_t(kind)], address, width, height, uint32_t(fetch.format), uint32_t(format), last.address,
              last.width, last.height, last.format, last.bytes >> 10, frame_ - last.frame);
}

void Textures::NoteResolveMiss(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch, VkFormat format,
                               uint32_t width, uint32_t height) {
  if (++stats_.resolve_misses > 20) {
    return;
  }
  std::string images;
  for (const auto& [key, resolved] : resolve_images_) {
    if (uint32_t(key >> 32) == address) {
      images += fmt::format(" {}x{} format {}", resolved.width, resolved.height, uint32_t(resolved.format));
    }
  }
  REXLOG_INFO("[native] texture {:08X} {}x{} guest format {} host {} read from guest memory; resolve images:{}",
              address, width, height, uint32_t(fetch.format), uint32_t(format), images);
}

void Textures::RecordLoad(const xenos::xe_gpu_texture_fetch_t& fetch, const LoadedTexture& loaded, uint32_t bytes,
                          UploadReason reason) {
  ++stats_.loads[size_t(reason)];
  stats_.load_bytes[size_t(reason)] += bytes;
  const UploadRecord record{uint32_t(fetch.base_address) << 12, uint32_t(fetch.format), loaded.width, loaded.height, bytes,
                            reason};
  const auto smaller = std::find_if(top_uploads_.begin(), top_uploads_.end(),
                                    [&](const UploadRecord& other) { return other.bytes < bytes; });
  if (top_uploads_.size() < kTopUploads) {
    top_uploads_.insert(smaller, record);
  } else if (smaller != top_uploads_.end()) {
    top_uploads_.insert(smaller, record);
    top_uploads_.pop_back();
  }
}

std::vector<Textures::UploadRecord> Textures::TakeTopUploads() { return std::exchange(top_uploads_, {}); }

void Textures::NoteEvicted(uint64_t key) {
  if (evicted_keys_.size() >= 65536) {
    evicted_keys_.clear();
  }
  evicted_keys_.insert(key);
}

void Textures::Retire(Texture& texture) {
  active_bytes_ -= texture.bytes;
  RetireImage(texture.image, texture.last_submission, texture.bytes, texture.shape);
}

void Textures::RetireImage(const Image& image, uint64_t submission, uint32_t bytes, uint64_t shape) {
  retired_.push_back({image, submission, bytes, shape});
  retired_bytes_ += bytes;
}

uint64_t Textures::ShapeKey(const TextureShape& loaded) {
  const std::array<uint32_t, 7> fields = {uint32_t(loaded.host.format), uint32_t(loaded.host.signed_format),
                                          loaded.width, loaded.height, loaded.mips, loaded.layers,
                                          uint32_t(loaded.cube)};
  return XXH3_64bits(fields.data(), sizeof(fields));
}

Textures::Image Textures::AcquireImage(const TextureShape& shape) {
  auto it = free_images_.find(ShapeKey(shape));
  if (it == free_images_.end()) {
    return CreateImage(shape);
  }
  Image image = it->second.image;
  free_bytes_ -= it->second.bytes;
  free_images_.erase(it);
  ++stats_.reused;
  return image;
}

void Textures::EnforceBudget() {
  const uint64_t budget = uint64_t(std::max(64, REXCVAR_GET(nier_texture_budget_mb))) << 20;
  while (active_bytes_ + free_bytes_ > budget && !free_images_.empty()) {
    auto it = free_images_.begin();
    Destroy(it->second.image);
    free_bytes_ -= it->second.bytes;
    free_images_.erase(it);
  }
  EvictOldest(budget);
}

void Textures::EvictOldest(uint64_t budget) {
  if (active_bytes_ <= budget) {
    return;
  }
  std::vector<EvictionCandidate> candidates;
  candidates.reserve(textures_.size());
  for (const auto& [key, texture] : textures_) {
    candidates.push_back({key, texture.used_ms, texture.bytes});
  }
  for (const uint64_t key : PickEvictions(std::move(candidates), active_bytes_, budget, now_ms_, kMinAgeMs)) {
    auto it = textures_.find(key);
    NoteEvicted(key);
    Retire(it->second);
    textures_.erase(it);
    ++stats_.evictions;
  }
}

void Textures::Evict(uint64_t unused_ms) {
  std::erase_if(textures_, [&](auto& entry) {
    Texture& texture = entry.second;
    if (texture.used_ms + unused_ms >= now_ms_) {
      return false;
    }
    NoteEvicted(entry.first);
    Retire(texture);
    ++stats_.evictions;
    return true;
  });
  std::erase_if(failed_, [&](const auto& entry) { return entry.second + kFailedRetryMs < now_ms_; });
  std::erase_if(resolve_records_,
                [&](const auto& entry) { return entry.second.frame + kRecentResolveFrames < frame_; });
}

void Textures::ReleaseMemory() {
  released_frame_ = frame_;
  ++stats_.releases;
  EvictOldest(0);
  for (Retired& retired : retired_) {
    Destroy(retired.image);
  }
  retired_.clear();
  retired_bytes_ = 0;
  for (auto& [shape, free] : free_images_) {
    Destroy(free.image);
  }
  free_images_.clear();
  free_bytes_ = 0;
}

void Textures::Completed(uint64_t submission) {
  std::erase_if(retired_, [&](Retired& retired) {
    if (retired.last_submission > submission) {
      return false;
    }
    retired_bytes_ -= retired.bytes;
    if (free_bytes_ + retired.bytes <= kFreeImageBytes) {
      free_images_.emplace(retired.shape, FreeImage{retired.image, retired.bytes});
      free_bytes_ += retired.bytes;
    } else {
      Destroy(retired.image);
    }
    return true;
  });
}

}  // namespace nier::native
