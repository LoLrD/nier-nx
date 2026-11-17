#include "native_renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

#include <fmt/format.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>
#include <rex/ui/vulkan/util.h>

#include "primitive_setup.h"
#include "resolve_plan.h"
#include "target_keys.h"

namespace nier::native {

using namespace rex::graphics;
namespace vk = rex::ui::vulkan;
using Translator = SpirvShaderTranslator;

namespace {

constexpr size_t kStagingPageBytes = 16 << 20;
constexpr VkDeviceSize kDrawUniformBytes = 16384;
constexpr uint64_t kEvictionPeriodFrames = 64;
constexpr uint64_t kUnusedFrames = 600;

}  // namespace

NativeRenderer::NativeRenderer(const vk::VulkanDevice* device, rex::memory::Memory* memory, NativeShaders& shaders)
    : device_(device),
      memory_(memory),
      physical_base_(memory->TranslatePhysical<uint8_t*>(0)),
      shaders_(shaders) {
  if (!device_->properties().dynamicRendering) {
    REXLOG_ERROR("[native] the device has no dynamic rendering, nothing will be drawn");
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  pipelines_ = std::make_unique<Pipelines>(device_);
  submissions_ = std::make_unique<Submissions>(device_, pipelines_->ConstantsLayout());
  mirror_ = std::make_unique<GuestMirror>(device_, physical_base_);
  if (!mirror_->valid()) {
    return;
  }
  targets_ = std::make_unique<RenderTargets>(device_);
  staging_ = std::make_unique<vk::VulkanUploadBufferPool>(device_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kStagingPageBytes);
  textures_ = std::make_unique<Textures>(device_, physical_base_, *staging_);
  resolves_ = std::make_unique<Resolves>(device_, *targets_, *textures_, physical_base_);
  const auto rectangles = BuildRectangleIndices(kMaxBuiltinRectangles);
  const auto quads = BuildQuadIndices(kMaxBuiltinQuads);
  quad_indices_offset_ = rectangles.size() * sizeof(uint32_t);
  const VkDeviceSize builtin_bytes = quad_indices_offset_ + quads.size() * sizeof(uint32_t);
  uint32_t builtin_type = 0;
  vk::util::CreateDedicatedAllocationBuffer(device_, builtin_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                            vk::util::MemoryPurpose::kUpload, builtin_indices_, builtin_memory_,
                                            &builtin_type);
  void* mapping = nullptr;
  dfn.vkMapMemory(d, builtin_memory_, 0, VK_WHOLE_SIZE, 0, &mapping);
  std::memcpy(mapping, rectangles.data(), quad_indices_offset_);
  std::memcpy(static_cast<uint8_t*>(mapping) + quad_indices_offset_, quads.data(), quads.size() * sizeof(uint32_t));
  vk::util::FlushMappedMemoryRange(device_, builtin_memory_, builtin_type);
  dfn.vkUnmapMemory(d, builtin_memory_);
  const uint32_t shared_count = pipelines_->shared_memory_binding_count();
  const VkDescriptorPoolSize shared_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, shared_count};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &shared_size;
  dfn.vkCreateDescriptorPool(d, &pool, nullptr, &shared_pool_);
  const VkDescriptorSetLayout shared_layout = pipelines_->SharedMemoryLayout();
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = shared_pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &shared_layout;
  dfn.vkAllocateDescriptorSets(d, &allocate, &shared_set_);
  std::vector<VkDescriptorBufferInfo> buffers(shared_count);
  const VkDeviceSize part = GuestMirror::kSize / shared_count;
  for (uint32_t i = 0; i < shared_count; ++i) {
    buffers[i] = {mirror_->buffer(), part * i, part};
  }
  const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   nullptr,
                                   shared_set_,
                                   0,
                                   0,
                                   shared_count,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   nullptr,
                                   buffers.data(),
                                   nullptr};
  dfn.vkUpdateDescriptorSets(d, 1, &write, 0, nullptr);
  VkFormatProperties output{};
  device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
      device_->physical_device(), vk::VulkanPresenter::kGuestOutputFormat, &output);
  output_blit_ = (output.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
  if (!output_blit_) {
    REXLOG_ERROR("[native] the guest output format cannot be a blit destination, frames stay black");
  }
  valid_ = true;
  REXLOG_INFO("[native] renderer ready: {} shared memory bindings, depth {}", shared_count,
              targets_->depth_format() == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
}

NativeRenderer::~NativeRenderer() {
  if (!submissions_) {
    return;
  }
  if (targets_ && submissions_->open()) {
    targets_->End(submissions_->main());
  }
  submissions_->WaitIdle();
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  dfn.vkDestroyDescriptorPool(d, shared_pool_, nullptr);
  dfn.vkDestroyBuffer(d, builtin_indices_, nullptr);
  dfn.vkFreeMemory(d, builtin_memory_, nullptr);
}

void NativeRenderer::BeginSubmission() {
  if (submissions_->open()) {
    return;
  }
  submissions_->Begin();
  textures_->Prepare(submissions_->upload());
  ProcessCompleted();
}

void NativeRenderer::Flush() {
  if (!submissions_->open()) {
    return;
  }
  targets_->End(submissions_->main());
  staging_->FlushWrites();
  submissions_->Submit();
}

void NativeRenderer::WaitFor(uint64_t submission) {
  if (submissions_->open() && submission >= submissions_->current()) {
    Flush();
  }
  submissions_->Wait(submission);
  ProcessCompleted();
}

void NativeRenderer::ProcessCompleted() {
  submissions_->Poll();
  const uint64_t completed = submissions_->completed();
  if (completed <= processed_) {
    return;
  }
  processed_ = completed;
  mirror_sync_.Completed(completed);
  textures_->Completed(completed);
  resolves_->Completed(completed);
  staging_->Reclaim(completed);
}

void NativeRenderer::SyncMirror(uint32_t address, uint32_t size) {
  if (!size || address >= GuestMirror::kSize) {
    return;
  }
  size = std::min(size, GuestMirror::kSize - address);
  const MirrorDecision decision = mirror_sync_.Use(address, size, physical_base_, submissions_->current());
  if (decision.wait_submission) {
    ++stats_.mirror_waits;
    WaitFor(decision.wait_submission);
    BeginSubmission();
    mirror_sync_.Use(address, size, physical_base_, submissions_->current());
  }
  if (decision.action == MirrorAction::kCopy) {
    mirror_->Copy(address, size);
    ++stats_.mirror_copies;
    stats_.mirror_bytes += size;
  }
}

VkDescriptorSet NativeRenderer::BindTextures(const RegisterFile& regs, const SpirvShader& shader, bool pixel,
                                             TextureConstantInfo& info, uint32_t& textures, uint32_t& samplers) {
  const auto& texture_bindings = shader.GetTextureBindingsAfterTranslation();
  const auto& sampler_bindings = shader.GetSamplerBindingsAfterTranslation();
  textures = uint32_t(texture_bindings.size());
  samplers = uint32_t(sampler_bindings.size());
  const VkDescriptorSet set = submissions_->Allocate(pipelines_->TextureSetLayout(pixel, textures, samplers));
  if (set == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  std::vector<VkDescriptorImageInfo> images(textures + samplers);
  std::vector<VkWriteDescriptorSet> writes(textures + samplers);
  for (uint32_t i = 0; i < textures; ++i) {
    const auto& binding = texture_bindings[i];
    const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
    const Textures::Binding bound = textures_->View(submissions_->upload(), fetch, binding, submissions_->current());
    images[i] = {VK_NULL_HANDLE, bound.view, VK_IMAGE_LAYOUT_GENERAL};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 &images[i], nullptr, nullptr};
    info.used_mask |= 1u << binding.fetch_constant;
    info.swizzled_signs[binding.fetch_constant] = bound.swizzled_signs;
    info.host_swizzles[binding.fetch_constant] = bound.host_swizzle;
  }
  for (uint32_t i = 0; i < samplers; ++i) {
    const auto& binding = sampler_bindings[i];
    const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
    images[textures + i] = {textures_->Sampler(fetch, binding), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    writes[textures + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, textures + i, 0, 1,
                            VK_DESCRIPTOR_TYPE_SAMPLER, &images[textures + i], nullptr, nullptr};
  }
  if (!writes.empty()) {
    device_->functions().vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0,
                                                nullptr);
  }
  return set;
}

void NativeRenderer::SetDynamicState(const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info,
                                     const SampleScale& scale, bool polygonal) {
  const auto& dfn = device_->functions();
  const VkCommandBuffer cmd = submissions_->main();
  VkViewport viewport{-1.0f, -1.0f, 1.0f, 1.0f, viewport_info.z_min, viewport_info.z_max};
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  }
  dfn.vkCmdSetViewport(cmd, 0, 1, &viewport);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const VkExtent2D area = targets_->render_area();
  const uint32_t left = std::min(scissor.offset[0] * scale.x, area.width);
  const uint32_t top = std::min(scissor.offset[1] * scale.y, area.height);
  const VkRect2D rect{{int32_t(left), int32_t(top)},
                      {std::min(scissor.extent[0] * scale.x, area.width - left),
                       std::min(scissor.extent[1] * scale.y, area.height - top)}};
  dfn.vkCmdSetScissor(cmd, 0, 1, &rect);
  float slope = 0.0f, constant = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope, constant);
  constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
                  ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                  : draw_util::kD3D10PolygonOffsetFactorFloat24;
  slope *= xenos::kPolygonOffsetScaleSubpixelUnit * float(std::max(scale.x, scale.y));
  dfn.vkCmdSetDepthBias(cmd, constant, 0.0f, slope);
  const float blend[4] = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                          regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  dfn.vkCmdSetBlendConstants(cmd, blend);
  const auto depth = draw_util::GetNormalizedDepthControl(regs);
  auto front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
  auto back = front;
  if (polygonal && depth.backface_enable) {
    back = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  }
  dfn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilmask);
  dfn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilmask);
  dfn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilwritemask);
  dfn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilwritemask);
  dfn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilref);
  dfn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilref);
}

void NativeRenderer::Draw(const RegisterFile& regs) {
  ++stats_.draws;
  if (!valid_) {
    return;
  }
  const bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) {
    ++stats_.skipped_raster;
    return;
  }
  const HostDraw draw = SetupPrimitive(regs);
  if (!draw.supported) {
    ++stats_.skipped_primitive;
    return;
  }
  if (!draw.host_count) {
    ++stats_.skipped_empty;
    return;
  }
  const SampleScale scale = ScaleFor(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
  const auto prepared = shaders_.PrepareDraw(regs, draw.vs_type, scale.x, scale.y);
  if (!prepared) {
    ++stats_.skipped_shader;
    return;
  }
  const uint32_t color_mask = draw_util::GetNormalizedColorMask(
      regs, prepared->pixel_shader ? prepared->pixel_shader->writes_color_targets() : 0);
  const TargetSet targets = TargetsFor(regs, color_mask);
  bool any = false;
  bool supported = true;
  for (const auto& color : targets.color) {
    if (color) {
      any = true;
      supported &= RenderTargets::Supported(*color);
    }
  }
  if (targets.depth) {
    any = true;
    supported &= RenderTargets::Supported(*targets.depth);
  }
  if (!any || !supported) {
    ++stats_.skipped_target;
    return;
  }
  BeginSubmission();
  if (!submissions_->HasUniformSpace(kDrawUniformBytes)) {
    Flush();
    BeginSubmission();
  }
  auto sync_ranges = [&] {
    for (const auto& binding : prepared->vertex_shader->vertex_bindings()) {
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      SyncMirror(fetch.address << 2, fetch.size << 2);
    }
    if (draw.index_source == IndexSource::kGuestDma) {
      SyncMirror(draw.guest_index_address, draw.guest_index_bytes);
    }
  };
  const uint64_t submission = submissions_->current();
  sync_ranges();
  if (submissions_->current() != submission) {
    sync_ranges();
  }
  TextureConstantInfo texture_info;
  uint32_t vs_textures = 0, vs_samplers = 0, ps_textures = 0, ps_samplers = 0;
  VkDescriptorSet vs_set = BindTextures(regs, *prepared->vertex_shader, false, texture_info, vs_textures, vs_samplers);
  VkDescriptorSet ps_set = prepared->pixel_shader
                               ? BindTextures(regs, *prepared->pixel_shader, true, texture_info, ps_textures, ps_samplers)
                               : submissions_->Allocate(pipelines_->TextureSetLayout(true, 0, 0));
  if (vs_set == VK_NULL_HANDLE || ps_set == VK_NULL_HANDLE) {
    Flush();
    ++stats_.skipped_shader;
    return;
  }
  PipelineState state = BuildPipelineState(regs, draw, targets, color_mask, targets_->depth_format());
  state.vertex_module = prepared->vertex->shader_module();
  state.pixel_module = prepared->pixel ? prepared->pixel->shader_module() : VK_NULL_HANDLE;
  const VkPipelineLayout layout = pipelines_->Layout(vs_textures, vs_samplers, ps_textures, ps_samplers);
  const VkPipeline pipeline = pipelines_->Get(state, layout);
  if (pipeline == VK_NULL_HANDLE) {
    ++stats_.skipped_shader;
    return;
  }
  const auto& properties = device_->properties();
  draw_util::ViewportInfo viewport;
  draw_util::GetHostViewportInfo(regs, targets.scale.x, targets.scale.y, false, properties.maxViewportDimensions[0],
                                 properties.maxViewportDimensions[1], true, draw_util::GetNormalizedDepthControl(regs),
                                 false, true, prepared->pixel_shader && prepared->pixel_shader->writes_depth(),
                                 viewport);
  Translator::SystemConstants system;
  FillSystemConstants(regs, draw, viewport, texture_info, system);
  alignas(16) uint8_t floats[Submissions::kFloatConstantsBytes];
  uint32_t bools[40];
  uint32_t fetches[192];
  std::array<uint32_t, Translator::kConstantBufferCount> offsets{};
  submissions_->Uniform(&system, sizeof(system), sizeof(system), offsets[Translator::kConstantBufferSystem]);
  uint32_t size = PackFloatConstants(regs, prepared->vertex_shader->constant_register_map(), false, floats);
  submissions_->Uniform(floats, size, Submissions::kFloatConstantsBytes, offsets[Translator::kConstantBufferFloatVertex]);
  size = prepared->pixel_shader
             ? PackFloatConstants(regs, prepared->pixel_shader->constant_register_map(), true, floats)
             : PackFloatConstants(regs, Shader::ConstantRegisterMap{}, true, floats);
  submissions_->Uniform(floats, size, Submissions::kFloatConstantsBytes, offsets[Translator::kConstantBufferFloatPixel]);
  CopyBoolLoopConstants(regs, bools);
  submissions_->Uniform(bools, sizeof(bools), sizeof(bools), offsets[Translator::kConstantBufferBoolLoop]);
  CopyFetchConstants(regs, fetches);
  submissions_->Uniform(fetches, sizeof(fetches), sizeof(fetches), offsets[Translator::kConstantBufferFetch]);
  const VkCommandBuffer cmd = submissions_->main();
  targets_->Begin(cmd, targets);
  if (!targets_->rendering()) {
    ++stats_.skipped_target;
    return;
  }
  const auto& dfn = device_->functions();
  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const std::array<VkDescriptorSet, 4> sets = {shared_set_, submissions_->constants_set(), vs_set, ps_set};
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, uint32_t(sets.size()), sets.data(),
                              uint32_t(offsets.size()), offsets.data());
  SetDynamicState(regs, viewport, targets.scale, polygonal);
  switch (draw.index_source) {
    case IndexSource::kGuestDma:
      dfn.vkCmdBindIndexBuffer(cmd, mirror_->buffer(), draw.guest_index_address,
                               draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      dfn.vkCmdDrawIndexed(cmd, draw.host_count, 1, 0, 0, 0);
      break;
    case IndexSource::kBuiltinRectangles:
      dfn.vkCmdBindIndexBuffer(cmd, builtin_indices_, 0, VK_INDEX_TYPE_UINT32);
      dfn.vkCmdDrawIndexed(cmd, draw.host_count, 1, 0, 0, 0);
      break;
    case IndexSource::kBuiltinQuads:
      dfn.vkCmdBindIndexBuffer(cmd, builtin_indices_, quad_indices_offset_, VK_INDEX_TYPE_UINT32);
      dfn.vkCmdDrawIndexed(cmd, draw.host_count, 1, 0, 0, 0);
      break;
    case IndexSource::kNone:
      dfn.vkCmdDraw(cmd, draw.host_count, 1, 0, 0);
      break;
  }
  ++stats_.drawn;
}

void NativeRenderer::Resolve(const RegisterFile& regs) {
  ++stats_.resolves;
  if (!valid_) {
    return;
  }
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) {
    return;
  }
  BeginSubmission();
  resolves_->Resolve(submissions_->main(), PlanResolve(info, regs), submissions_->current());
}

void NativeRenderer::PresentInto(VkCommandBuffer cmd, VkImage output, bool written_before, uint32_t frontbuffer,
                                 uint32_t width, uint32_t height) {
  const auto& dfn = device_->functions();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = output;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(cmd,
                           written_before ? vk::VulkanPresenter::kGuestOutputInternalStageMask
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  uint32_t source_width = 0, source_height = 0;
  const VkImage source = resolves_->FindImage(frontbuffer, source_width, source_height);
  if (source != VK_NULL_HANDLE && output_blit_) {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(std::min(width, source_width)), int32_t(std::min(height, source_height)), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(width), int32_t(height), 1};
    dfn.vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, output, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_LINEAR);
  } else {
    ++stats_.black_frames;
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn.vkCmdClearColorImage(cmd, output, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = vk::VulkanPresenter::kGuestOutputInternalAccessMask;
  barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.newLayout = vk::VulkanPresenter::kGuestOutputInternalLayout;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, vk::VulkanPresenter::kGuestOutputInternalStageMask, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
}

void NativeRenderer::SetFrameDump(std::string directory, uint32_t interval) {
  dump_directory_ = std::move(directory);
  dump_interval_ = interval;
}

void NativeRenderer::DumpFrame(uint32_t frontbuffer, uint32_t width, uint32_t height) {
  uint32_t image_width = 0, image_height = 0;
  const VkImage source = resolves_->FindImage(frontbuffer, image_width, image_height);
  width = std::min(width, image_width);
  height = std::min(height, image_height);
  if (source == VK_NULL_HANDLE || !width || !height) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (!vk::util::CreateDedicatedAllocationBuffer(device_, VkDeviceSize(width) * height * 4,
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::util::MemoryPurpose::kReadback,
                                                 buffer, memory)) {
    return;
  }
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  dfn.vkCmdCopyImageToBuffer(submissions_->main(), source, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
  FullBarrier(device_, submissions_->main());
  const uint64_t submission = submissions_->current();
  WaitFor(submission);
  void* mapping = nullptr;
  dfn.vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &mapping);
  const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory, 0, VK_WHOLE_SIZE};
  dfn.vkInvalidateMappedMemoryRanges(d, 1, &range);
  const std::string path = fmt::format("{}/frame_{:06}.ppm", dump_directory_, stats_.swaps);
  std::ofstream file(path, std::ios::binary);
  file << "P6\n" << width << " " << height << "\n255\n";
  const auto* pixels = static_cast<const uint8_t*>(mapping);
  std::vector<uint8_t> row(size_t(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      std::memcpy(&row[size_t(x) * 3], pixels + (size_t(y) * width + x) * 4, 3);
    }
    file.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
  }
  dfn.vkUnmapMemory(d, memory);
  dfn.vkDestroyBuffer(d, buffer, nullptr);
  dfn.vkFreeMemory(d, memory, nullptr);
  BeginSubmission();
}

void NativeRenderer::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height) {
  ++stats_.swaps;
  if (!valid_) {
    return;
  }
  BeginSubmission();
  targets_->End(submissions_->main());
  if (dump_interval_ && stats_.swaps % dump_interval_ == 0) {
    DumpFrame(frontbuffer, width, height);
  }
  if (presenter && width && height) {
    presenter->RefreshGuestOutput(width, height, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
      auto& output = static_cast<vk::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
      PresentInto(submissions_->main(), output.image(), output.image_ever_written_previously(), frontbuffer, width,
                  height);
      Flush();
      return true;
    });
  }
  Flush();
  mirror_sync_.BeginFrame();
  textures_->BeginFrame();
  resolves_->BeginFrame();
  if (stats_.swaps % kEvictionPeriodFrames == 0) {
    mirror_sync_.Prune(kUnusedFrames);
    textures_->Evict(kUnusedFrames);
    resolves_->Evict(kUnusedFrames, submissions_->current());
  }
  ProcessCompleted();
}

std::string NativeRenderer::Report() {
  if (!valid_) {
    return "[native] renderer disabled";
  }
  const Stats& s = stats_;
  const Stats& r = reported_;
  const Textures::Stats& t = textures_->stats();
  const Resolves::Stats& v = resolves_->stats();
  const uint64_t submissions = submissions_->current() - 1;
  std::string line = fmt::format(
      "[native] render: {}/{} draws drawn, skipped raster {} primitive {} target {} shader {} empty {}; {} resolves ({} colour, "
      "{} depth, {} clears, {} readbacks, {} skipped); mirror {} copies {} MB {} waits; textures {} uploads {} MB {} "
      "unsupported {} resolve hits, {} live; {} resolve images, {} mirror keys; {} targets, {} pipelines, {} submissions, {} black frames",
      s.drawn - r.drawn, s.draws - r.draws, s.skipped_raster - r.skipped_raster,
      s.skipped_primitive - r.skipped_primitive, s.skipped_target - r.skipped_target,
      s.skipped_shader - r.skipped_shader, s.skipped_empty - r.skipped_empty, s.resolves - r.resolves, v.copies - reported_resolves_.copies,
      v.depth_copies - reported_resolves_.depth_copies, v.clears - reported_resolves_.clears,
      v.readbacks - reported_resolves_.readbacks, v.skipped - reported_resolves_.skipped,
      s.mirror_copies - r.mirror_copies, (s.mirror_bytes - r.mirror_bytes) >> 20, s.mirror_waits - r.mirror_waits,
      t.uploads - reported_textures_.uploads, (t.upload_bytes - reported_textures_.upload_bytes) >> 20,
      t.unsupported - reported_textures_.unsupported, t.resolve_hits - reported_textures_.resolve_hits,
      textures_->live_images(), resolves_->image_count(), mirror_sync_.key_count(), targets_->count(), pipelines_->pipeline_count(), submissions - reported_submissions_,
      s.black_frames - r.black_frames);
  reported_ = s;
  reported_textures_ = t;
  reported_resolves_ = v;
  reported_submissions_ = submissions;
  return line;
}

}  // namespace nier::native
