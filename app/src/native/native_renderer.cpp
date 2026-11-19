#include "native_renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>
#include <rex/ui/vulkan/util.h>

#include "perf_modes.h"
#include "primitive_setup.h"
#include "shaders/apply_gamma_cs.h"
#include "resolve_plan.h"
#include "target_keys.h"

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace nier::native {

using namespace rex::graphics;
namespace vk = rex::ui::vulkan;
using Translator = SpirvShaderTranslator;

namespace {

class ScopedMicros {
 public:
  explicit ScopedMicros(uint64_t& total) : total_(total), start_(std::chrono::steady_clock::now()) {}
  ~ScopedMicros() {
    total_ += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
  }

 private:
  uint64_t& total_;
  std::chrono::steady_clock::time_point start_;
};

constexpr VkDeviceSize kMemoryProbeBytes = 8 << 20;

double WriteSpeed(const vk::VulkanDevice* device, vk::util::MemoryPurpose purpose, const std::vector<uint8_t>& source,
                  uint32_t& memory_type) {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (!vk::util::CreateDedicatedAllocationBuffer(device, kMemoryProbeBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, purpose,
                                                 buffer, memory, &memory_type)) {
    return 0.0;
  }
  const auto& dfn = device->functions();
  void* mapping = nullptr;
  dfn.vkMapMemory(device->device(), memory, 0, VK_WHOLE_SIZE, 0, &mapping);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 4; ++i) {
    std::memcpy(mapping, source.data(), source.size());
    vk::util::FlushMappedMemoryRange(device, memory, memory_type, 0, kMemoryProbeBytes, kMemoryProbeBytes);
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  dfn.vkDestroyBuffer(device->device(), buffer, nullptr);
  dfn.vkFreeMemory(device->device(), memory, nullptr);
  return 4.0 * double(kMemoryProbeBytes >> 20) / seconds;
}

bool PreferCachedUploads(const vk::VulkanDevice* device) {
  const std::vector<uint8_t> source(kMemoryProbeBytes, 0x5A);
  uint32_t uncached_type = UINT32_MAX, cached_type = UINT32_MAX;
  const double uncached = WriteSpeed(device, vk::util::MemoryPurpose::kUpload, source, uncached_type);
  const double cached = WriteSpeed(device, vk::util::MemoryPurpose::kReadback, source, cached_type);
  const bool prefer = cached_type != uncached_type && cached > uncached * 1.25;
  REXLOG_INFO("[native] host writes: type {} {:.0f} MB/s, type {} {:.0f} MB/s (flushed), using {}", uncached_type,
              uncached, cached_type, cached, prefer ? "cached" : "uncached");
  return prefer;
}

constexpr size_t kStagingPageBytes = 16 << 20;
constexpr VkDeviceSize kDrawUniformBytes = 16384;
constexpr uint64_t kStagingFlushBytes = 16ull << 20;
constexpr uint64_t kEvictionPeriodMs = 1000;
constexpr uint64_t kUnusedSeconds = 10;
constexpr size_t kReportedPasses = 8;
constexpr size_t kReportedShaders = 10;
constexpr size_t kTracedFrames = 12;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

NativeRenderer::NativeRenderer(const vk::VulkanDevice* device, rex::memory::Memory* memory, NativeShaders& shaders,
                               const GpuRegisters& registers)
    : device_(device),
      memory_(memory),
      physical_base_(memory->TranslatePhysical<uint8_t*>(0)),
      shaders_(shaders),
      registers_(registers) {
  if (!device_->properties().dynamicRendering) {
    REXLOG_ERROR("[native] the device has no dynamic rendering, nothing will be drawn");
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  pipelines_ = std::make_unique<Pipelines>(device_);
  const bool cached_uploads = PreferCachedUploads(device_);
  submissions_ = std::make_unique<Submissions>(device_, pipelines_->ConstantsLayout(), cached_uploads);
  mirror_ = std::make_unique<GuestMirror>(device_, physical_base_, cached_uploads);
  if (!mirror_->valid()) {
    return;
  }
  targets_ = std::make_unique<RenderTargets>(device_);
  staging_ = std::make_unique<vk::VulkanUploadBufferPool>(device_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kStagingPageBytes,
                                                         cached_uploads);
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
  CreateGammaPipeline();
  last_swap_ms_ = NowMs();
  valid_ = true;
  REXLOG_INFO("[native] renderer ready: {} shared memory bindings, depth {}", shared_count,
              targets_->depth_format() == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
}

void NativeRenderer::CreateGammaPipeline() {
  const auto& dfn = device_->functions();
  const VkDevice d = device_->device();
  const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  }};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = uint32_t(bindings.size());
  set_info.pBindings = bindings.data();
  dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &gamma_set_layout_);
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(int32_t)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &gamma_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &gamma_layout_);
  const VkShaderModule module = vk::util::CreateShaderModule(device_, apply_gamma_cs, sizeof(apply_gamma_cs));
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                         module, "main", nullptr};
  pipeline_info.layout = gamma_layout_;
  dfn.vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &gamma_pipeline_);
  dfn.vkDestroyShaderModule(d, module, nullptr);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_NEAREST;
  sampler.minFilter = VK_FILTER_NEAREST;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dfn.vkCreateSampler(d, &sampler, nullptr, &gamma_sampler_);
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
  dfn.vkDestroySampler(d, gamma_sampler_, nullptr);
  dfn.vkDestroyPipeline(d, gamma_pipeline_, nullptr);
  dfn.vkDestroyPipelineLayout(d, gamma_layout_, nullptr);
  dfn.vkDestroyDescriptorSetLayout(d, gamma_set_layout_, nullptr);
  dfn.vkDestroyDescriptorPool(d, shared_pool_, nullptr);
  dfn.vkDestroyBuffer(d, builtin_indices_, nullptr);
  dfn.vkFreeMemory(d, builtin_memory_, nullptr);
}

void NativeRenderer::BeginSubmission() {
  if (submissions_->open()) {
    return;
  }
  {
    ScopedMicros timer(stats_.us_gpu_wait);
    submissions_->Begin();
  }
  textures_->ResetStaged();
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
  {
    ScopedMicros timer(stats_.us_gpu_wait);
    submissions_->Wait(submission);
  }
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
  ScopedMicros timer(stats_.us_mirror);
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
                                             TextureConstantInfo& info, uint32_t& textures, uint32_t& samplers,
                                             bool& out_of_memory) {
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
    Textures::Binding bound;
    {
      ScopedMicros timer(stats_.us_textures);
      bound = textures_->View(submissions_->upload(), fetch, binding, submissions_->current());
    }
    if (bound.out_of_memory) {
      out_of_memory = true;
      return VK_NULL_HANDLE;
    }
    if (bound.resolve_address) {
      trace_pass_.samples.insert({bound.resolve_address, (fetch.size_2d.width + 1) << 16 | (fetch.size_2d.height + 1)});
    }
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

VkRect2D NativeRenderer::SetDynamicState(const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info,
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
  VkRect2D host_rect = rect;
  if (PerfSkip(kSkipPixels)) {
    host_rect.extent = {0, 0};
  } else if (PerfSkip(kQuarterScreen)) {
    host_rect.extent = {rect.extent.width / 2, rect.extent.height / 2};
  }
  dfn.vkCmdSetScissor(cmd, 0, 1, &host_rect);
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
  return rect;
}

void NativeRenderer::Draw(const RegisterFile& regs) {
  ++stats_.draws;
  DelayDraw();
  ScopedMicros timer(stats_.us_draws);
  if (!valid_) {
    return;
  }
  DrawResult result = RecordDraw(regs);
  if (result == DrawResult::kDone) {
    return;
  }
  ++stats_.draw_retries;
  Flush();
  if (result == DrawResult::kOutOfMemory) {
    ReleaseMemory();
  }
  if (RecordDraw(regs) != DrawResult::kDone) {
    ++stats_.skipped_retry;
  }
}

void NativeRenderer::ReleaseMemory() {
  ++stats_.memory_releases;
  {
    ScopedMicros timer(stats_.us_gpu_wait);
    submissions_->WaitIdle();
  }
  ProcessCompleted();
  textures_->ReleaseMemory();
  REXLOG_WARN("[native] out of memory: waited for the GPU and released textures, {} MB still in use",
                 textures_->active_bytes() >> 20);
}

NativeRenderer::DrawResult NativeRenderer::RecordDraw(const RegisterFile& regs) {
  if (PerfSkip(kSkipDraws)) {
    return DrawResult::kDone;
  }
  const bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) {
    ++stats_.skipped_raster;
    return DrawResult::kDone;
  }
  const HostDraw draw = SetupPrimitive(regs);
  if (!draw.supported) {
    ++stats_.skipped_primitive;
    return DrawResult::kDone;
  }
  if (!draw.host_count) {
    ++stats_.skipped_empty;
    return DrawResult::kDone;
  }
  const SampleScale scale = ScaleFor(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
  const auto prepared = shaders_.PrepareDraw(regs, draw.vs_type, scale.x, scale.y);
  if (!prepared) {
    ++stats_.skipped_shader;
    return DrawResult::kDone;
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
    return DrawResult::kDone;
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
  bool out_of_memory = false;
  const VkDescriptorSet vs_set =
      BindTextures(regs, *prepared->vertex_shader, false, texture_info, vs_textures, vs_samplers, out_of_memory);
  if (vs_set == VK_NULL_HANDLE) {
    return out_of_memory ? DrawResult::kOutOfMemory : DrawResult::kRetry;
  }
  const VkDescriptorSet ps_set =
      prepared->pixel_shader
          ? BindTextures(regs, *prepared->pixel_shader, true, texture_info, ps_textures, ps_samplers, out_of_memory)
          : submissions_->Allocate(pipelines_->TextureSetLayout(true, 0, 0));
  if (ps_set == VK_NULL_HANDLE) {
    return out_of_memory ? DrawResult::kOutOfMemory : DrawResult::kRetry;
  }
  PipelineState state = BuildPipelineState(regs, draw, targets, color_mask, targets_->depth_format());
  state.vertex_module = prepared->vertex->shader_module();
  state.pixel_module = prepared->pixel ? prepared->pixel->shader_module() : VK_NULL_HANDLE;
  if (PerfSkip(kSkipPixelShaders)) {
    state.pixel_module = VK_NULL_HANDLE;
  }
  const VkPipelineLayout layout = pipelines_->Layout(vs_textures, vs_samplers, ps_textures, ps_samplers);
  const VkPipeline pipeline = pipelines_->Get(state, layout);
  if (pipeline == VK_NULL_HANDLE) {
    ++stats_.skipped_shader;
    return DrawResult::kDone;
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
  if (targets_->Begins(targets)) {
    targets_->End(cmd);
    current_pass_ = PassId(targets);
    submissions_->Mark(GpuCategory::kDraws, LabelId(current_pass_, nullptr, nullptr));
    FlushTracePass();
    trace_pass_.pass = current_pass_;
    frame_passes_.push_back(current_pass_);
  }
  targets_->Begin(cmd, targets);
  if (!targets_->rendering()) {
    ++stats_.skipped_target;
    return DrawResult::kDone;
  }
  const auto& dfn = device_->functions();
  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  submissions_->Mark(GpuCategory::kDraws, LabelId(current_pass_, prepared->pixel_shader, prepared->pixel));
  const std::array<VkDescriptorSet, 4> sets = {shared_set_, submissions_->constants_set(), vs_set, ps_set};
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, uint32_t(sets.size()), sets.data(),
                              uint32_t(offsets.size()), offsets.data());
  const VkRect2D scissor = SetDynamicState(regs, viewport, targets.scale, polygonal);
  if (!trace_pass_.draws++) {
    trace_pass_.first_scissor = scissor;
  }
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
  if (textures_->staged_bytes() > kStagingFlushBytes) {
    ++stats_.staging_flushes;
    Flush();
  }
  return DrawResult::kDone;
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
  ScopedMicros timer(stats_.us_resolves);
  targets_->End(submissions_->main());
  submissions_->Mark(GpuCategory::kResolves);
  const ResolvePlan plan = PlanResolve(info, regs);
  FlushTracePass();
  trace_.push_back(fmt::format(
      "resolve [{} {:X} w{} f{}] {},{} {}x{} -> {:08X} f{} {}x{} pitch {}{}{}{}{}{}", plan.source.is_depth ? "d" : "c",
      plan.source.base_tiles, ExtentFor(plan.source).width, plan.source.format, plan.source_x, plan.source_y,
      plan.source_width, plan.source_height, plan.dest_address, uint32_t(plan.dest_format), plan.dest_width,
      plan.dest_height, plan.dest_pitch, plan.copy ? " copy" : "", plan.depth ? " depth" : "",
      plan.clear_color ? " clear-colour" : "", plan.clear_depth ? " clear-depth" : "", plan.readback ? " readback" : "",
      plan.dest_swap ? " swap" : ""));
  resolves_->Resolve(submissions_->main(), plan, submissions_->current());
}

void NativeRenderer::PresentInto(VkCommandBuffer cmd, VkImage output, VkImageView output_view, bool written_before,
                                 uint32_t frontbuffer, uint32_t width, uint32_t height) {
  const auto& dfn = device_->functions();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = output;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(cmd,
                           written_before ? vk::VulkanPresenter::kGuestOutputInternalStageMask
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
  uint32_t source_width = 0, source_height = 0;
  VkImageView source_view = VK_NULL_HANDLE;
  VkFormat source_format = VK_FORMAT_UNDEFINED;
  const VkImage source =
      resolves_->FindImage(frontbuffer, source_width, source_height, &source_view, &source_format);
  const Presented presented{source != VK_NULL_HANDLE, source_format, source_width, source_height};
  if (presented != presented_ && ++stats_.presented_changes <= 200) {
    REXLOG_INFO("[native] swap {} presents {:08X}: {}", stats_.swaps, frontbuffer,
                presented.found ? fmt::format("{}x{} format {}", source_width, source_height, uint32_t(source_format))
                                : "no image");
  }
  presented_ = presented;
  uint32_t ramp_offset = 0;
  const VkDescriptorSet set = source != VK_NULL_HANDLE ? submissions_->Allocate(gamma_set_layout_) : VK_NULL_HANDLE;
  if (set != VK_NULL_HANDLE &&
      submissions_->Uniform(registers_.gamma_ramp().data(), sizeof(uint32_t) * 256, sizeof(uint32_t) * 256,
                            ramp_offset)) {
    const VkDescriptorImageInfo source_info{gamma_sampler_, source_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo output_info{VK_NULL_HANDLE, output_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo ramp_info{submissions_->uniform_buffer(), ramp_offset, sizeof(uint32_t) * 256};
    const std::array<VkWriteDescriptorSet, 3> writes = {{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         &source_info, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output_info,
         nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr,
         &ramp_info, nullptr},
    }};
    dfn.vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    const int32_t size[2] = {int32_t(std::min(width, source_width)), int32_t(std::min(height, source_height))};
    dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline_);
    dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gamma_layout_, 0, 1, &set, 0, nullptr);
    dfn.vkCmdPushConstants(cmd, gamma_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    dfn.vkCmdDispatch(cmd, (uint32_t(size[0]) + 7) / 8, (uint32_t(size[1]) + 7) / 8, 1);
  } else {
    if (++stats_.black_frames <= 20) {
      REXLOG_INFO("[native] no frontbuffer image at {:08X} for swap {}", frontbuffer, stats_.swaps);
    }
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn.vkCmdClearColorImage(cmd, output, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = vk::VulkanPresenter::kGuestOutputInternalAccessMask;
  barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.newLayout = vk::VulkanPresenter::kGuestOutputInternalLayout;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           vk::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
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
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  if (!valid_) {
    return;
  }
  TraceSwap(frontbuffer, width, height);
  BeginSubmission();
  ScopedMicros timer(stats_.us_present);
  targets_->End(submissions_->main());
  submissions_->Mark(GpuCategory::kPresent);
  if (dump_interval_ && stats_.swaps % dump_interval_ == 0) {
    DumpFrame(frontbuffer, width, height);
  }
  if (presenter && width && height) {
    uint64_t us_paint = 0;
    uint64_t us_refresh = 0;
    {
      ScopedMicros refresh_timer(us_refresh);
      presenter->RefreshGuestOutput(width, height, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        ScopedMicros paint_timer(us_paint);
        auto& output = static_cast<vk::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        PresentInto(submissions_->main(), output.image(), output.image_view(), output.image_ever_written_previously(),
                    frontbuffer, width, height);
        Flush();
        return true;
      });
    }
    stats_.us_presenter += us_refresh - std::min(us_refresh, us_paint);
  }
  Flush();
  const uint64_t now_ms = NowMs();
  frame_clock_ms_ = AdvanceFrameClock(frame_clock_ms_, last_swap_ms_, now_ms);
  last_swap_ms_ = now_ms;
  mirror_sync_.BeginFrame();
  textures_->BeginFrame(frame_clock_ms_);
  resolves_->BeginFrame();
  if (frame_clock_ms_ >= last_eviction_ms_ + kEvictionPeriodMs) {
    last_eviction_ms_ = frame_clock_ms_;
    textures_->Evict(kUnusedSeconds * 1000);
    swap_marks_.push_back(stats_.swaps);
    if (swap_marks_.size() > kUnusedSeconds + 1) {
      swap_marks_.pop_front();
    }
    if (swap_marks_.size() == kUnusedSeconds + 1) {
      const uint64_t unused_frames = stats_.swaps - swap_marks_.front();
      mirror_sync_.Prune(unused_frames);
      resolves_->Evict(unused_frames, submissions_->current());
    }
  }
  ProcessCompleted();
}

uint16_t NativeRenderer::PassId(const TargetSet& set) {
  std::string name;
  for (uint32_t i = 0; i < set.color.size(); ++i) {
    if (const auto& key = set.color[i]) {
      name += fmt::format("c{} {:X} w{} f{} ", i, key->base_tiles, ExtentFor(*key).width, key->format);
    }
  }
  if (set.depth) {
    name += fmt::format("d {:X} w{} f{} ", set.depth->base_tiles, ExtentFor(*set.depth).width, set.depth->format);
  }
  name += fmt::format("msaa {}x{}", set.scale.x, set.scale.y);
  auto [it, inserted] = pass_ids_.try_emplace(name, uint16_t(pass_names_.size()));
  if (inserted) {
    pass_names_.push_back(name);
  }
  return it->second;
}

uint16_t NativeRenderer::LabelId(uint16_t pass, const vulkan::VulkanShader* pixel_shader,
                                 const vulkan::VulkanShader::VulkanTranslation* pixel) {
  const uint64_t hash = pixel_shader ? pixel_shader->ucode_data_hash() | 1 : 0;
  const uint64_t key = (uint64_t(pass) << 48) ^ (hash & 0xFFFFFFFFFFFFull);
  auto [it, inserted] = label_ids_.try_emplace(key, uint16_t(label_pass_.size()));
  if (inserted) {
    label_pass_.push_back(pass);
    label_shader_.push_back(pixel_shader ? fmt::format("ps {:016X} ({} ucode dwords, {} spirv words)", hash,
                                                       pixel_shader->ucode_dword_count(),
                                                       pixel ? pixel->translated_binary().size() / 4 : 0)
                                         : std::string());
  }
  return it->second;
}

void NativeRenderer::FlushTracePass() {
  if (trace_pass_.draws) {
    std::string samples;
    for (const auto& [address, size] : trace_pass_.samples) {
      samples += fmt::format(" {:08X} {}x{}", address, size >> 16, size & 0xFFFF);
    }
    const VkRect2D& r = trace_pass_.first_scissor;
    trace_.push_back(fmt::format("pass [{}] {} draws, first scissor {},{} {}x{}, samples resolves:{}",
                                 pass_names_[trace_pass_.pass], trace_pass_.draws, r.offset.x, r.offset.y,
                                 r.extent.width, r.extent.height, samples.empty() ? " none" : samples));
  }
  trace_pass_ = {};
  trace_pass_.pass = current_pass_;
}

void NativeRenderer::TraceSwap(uint32_t frontbuffer, uint32_t width, uint32_t height) {
  FlushTracePass();
  trace_.push_back(fmt::format("swap {:08X} {}x{}", frontbuffer, width, height));
  std::vector<uint16_t> signature = frame_passes_;
  std::sort(signature.begin(), signature.end());
  signature.erase(std::unique(signature.begin(), signature.end()), signature.end());
  if (traced_frames_.size() < kTracedFrames && traced_frames_.insert(signature).second) {
    for (const std::string& line : trace_) {
      REXLOG_INFO("[native] trace {}: {}", stats_.swaps, line);
    }
  }
  trace_.clear();
  frame_passes_.clear();
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
      "{} depth, {} clears, {} readbacks, {} stale, {} skipped); mirror {} copies {} MB {} waits; textures {} uploads {} MB {} "
      "unsupported {} resolve hits {} reused, {} live {} MB; {} resolve images, {} mirror keys; {} targets, {} pipelines, {} submissions, {} black frames",
      s.drawn - r.drawn, s.draws - r.draws, s.skipped_raster - r.skipped_raster,
      s.skipped_primitive - r.skipped_primitive, s.skipped_target - r.skipped_target,
      s.skipped_shader - r.skipped_shader, s.skipped_empty - r.skipped_empty, s.resolves - r.resolves, v.copies - reported_resolves_.copies,
      v.depth_copies - reported_resolves_.depth_copies, v.clears - reported_resolves_.clears,
      v.readbacks - reported_resolves_.readbacks, v.stale_readbacks - reported_resolves_.stale_readbacks,
      v.skipped - reported_resolves_.skipped,
      s.mirror_copies - r.mirror_copies, (s.mirror_bytes - r.mirror_bytes) >> 20, s.mirror_waits - r.mirror_waits,
      t.uploads - reported_textures_.uploads, (t.upload_bytes - reported_textures_.upload_bytes) >> 20,
      t.unsupported - reported_textures_.unsupported, t.resolve_hits - reported_textures_.resolve_hits,
      t.reused - reported_textures_.reused,
      textures_->live_images(), (textures_->active_bytes() + textures_->retired_bytes() + textures_->free_bytes()) >> 20, resolves_->image_count(), mirror_sync_.key_count(), targets_->count(), pipelines_->pipeline_count(), submissions - reported_submissions_,
      s.black_frames - r.black_frames);
  line += fmt::format("; ms draws {} (mirror {}, textures {}), resolves {}, present {}, gpu wait {}",
                      (s.us_draws - r.us_draws) / 1000, (s.us_mirror - r.us_mirror) / 1000,
                      (s.us_textures - r.us_textures) / 1000, (s.us_resolves - r.us_resolves) / 1000,
                      (s.us_present - r.us_present) / 1000, (s.us_gpu_wait - r.us_gpu_wait) / 1000);
  const auto& gpu = submissions_->gpu_ns();
  auto gpu_ms = [&](GpuCategory category) {
    return (gpu[size_t(category)] - reported_gpu_ns_[size_t(category)]) / 1000000;
  };
  line += fmt::format(
      "; cpu: mirror hashed {} MB ({} sampled checks found {} changes, {} possibly late), textures hashed {} MB ({} "
      "sampled checks found {} changes, {} possibly late), presenter {} ms",
      (mirror_sync_.hashed_bytes() - reported_mirror_.hashed) >> 20,
      mirror_sync_.sampled_checks() - reported_mirror_.sampled,
      mirror_sync_.sampled_changes() - reported_mirror_.sampled_changes,
      mirror_sync_.possibly_late() - reported_mirror_.possibly_late,
      (t.hashed_bytes - reported_textures_.hashed_bytes) >> 20, t.sampled_checks - reported_textures_.sampled_checks,
      t.sampled_changes - reported_textures_.sampled_changes, t.possibly_late - reported_textures_.possibly_late,
      (s.us_presenter - r.us_presenter) / 1000);
  reported_mirror_ = {mirror_sync_.hashed_bytes(), mirror_sync_.sampled_checks(), mirror_sync_.sampled_changes(),
                      mirror_sync_.possibly_late()};
  line += fmt::format("; gpu ms draws {}, resolves {}, uploads {}, present {}", gpu_ms(GpuCategory::kDraws),
                      gpu_ms(GpuCategory::kResolves), gpu_ms(GpuCategory::kUploads), gpu_ms(GpuCategory::kPresent));
  reported_gpu_ns_ = gpu;
  const std::vector<uint64_t>& labels = submissions_->pass_ns();
  reported_pass_ns_.resize(labels.size());
  std::vector<uint64_t> per_pass(pass_names_.size());
  std::vector<std::pair<uint64_t, size_t>> shaders;
  for (size_t i = 1; i < labels.size(); ++i) {
    const uint64_t ns = labels[i] - reported_pass_ns_[i];
    if (!ns) {
      continue;
    }
    per_pass[label_pass_[i]] += ns;
    if (!label_shader_[i].empty()) {
      shaders.emplace_back(ns, i);
    }
  }
  std::vector<std::pair<uint64_t, size_t>> heaviest;
  for (size_t i = 1; i < per_pass.size(); ++i) {
    if (per_pass[i]) {
      heaviest.emplace_back(per_pass[i], i);
    }
  }
  std::sort(heaviest.rbegin(), heaviest.rend());
  heaviest.resize(std::min(heaviest.size(), kReportedPasses));
  if (!heaviest.empty()) {
    line += "; gpu passes:";
    for (const auto& [ns, id] : heaviest) {
      line += fmt::format(" {} ms [{}],", ns / 1000000, pass_names_[id]);
    }
    line.pop_back();
  }
  std::sort(shaders.rbegin(), shaders.rend());
  shaders.resize(std::min(shaders.size(), kReportedShaders));
  if (!shaders.empty()) {
    line += "; gpu shaders:";
    for (const auto& [ns, id] : shaders) {
      line += fmt::format(" {} ms [{}] {},", ns / 1000000, pass_names_[label_pass_[id]], label_shader_[id]);
    }
    line.pop_back();
  }
  reported_pass_ns_ = labels;
  const Textures::Stats& rt = reported_textures_;
  line += fmt::format("; loads new {} ({} MB), changed {} ({} MB), evicted {} ({} MB), {} resolve misses",
                      t.loads[0] - rt.loads[0], (t.load_bytes[0] - rt.load_bytes[0]) >> 20, t.loads[1] - rt.loads[1],
                      (t.load_bytes[1] - rt.load_bytes[1]) >> 20, t.loads[2] - rt.loads[2],
                      (t.load_bytes[2] - rt.load_bytes[2]) >> 20, t.resolve_misses - rt.resolve_misses);
  line += fmt::format("; stale reads: older images {}, forgotten {}, removed {}, inside ranges {}",
                      t.stale_reads[1] - rt.stale_reads[1], t.stale_reads[2] - rt.stale_reads[2],
                      t.stale_reads[3] - rt.stale_reads[3], t.stale_reads[4] - rt.stale_reads[4]);
  constexpr const char* kReasons[] = {"new", "changed", "evicted"};
  const auto top = textures_->TakeTopUploads();
  if (!top.empty()) {
    line += "; top uploads:";
    for (const auto& upload : top) {
      line += fmt::format(" {:08X} format {} {}x{} {} KB {},", upload.address, upload.format, upload.width,
                          upload.height, upload.bytes >> 10, kReasons[size_t(upload.reason)]);
    }
    line.pop_back();
  }
  const RenderTargets::Stats& e = targets_->stats();
  line += fmt::format("; edram transfers {} ({} tiles in {} regions), {} incompatible tiles",
                      e.transfers - reported_targets_.transfers, e.transfer_tiles - reported_targets_.transfer_tiles,
                      e.transfer_regions - reported_targets_.transfer_regions,
                      e.incompatible_tiles - reported_targets_.incompatible_tiles);
  reported_targets_ = e;
  line += fmt::format("; {} evictions, {} memory skips, {} releases, {} draw retries, {} skipped after retry, {} staging flushes",
                      t.evictions - rt.evictions, t.memory_failures - rt.memory_failures, t.releases - rt.releases,
                      s.draw_retries - r.draw_retries, s.skipped_retry - r.skipped_retry,
                      s.staging_flushes - r.staging_flushes);
  line += fmt::format("; memory MB: mirror {}, textures {} (retired {}, free {}), resolve images {}, targets {}, staging {}, uniforms {}",
                      GuestMirror::kSize >> 20, textures_->active_bytes() >> 20, textures_->retired_bytes() >> 20,
                      textures_->free_bytes() >> 20,
                      resolves_->bytes() >> 20, targets_->bytes() >> 20,
                      (staging_->page_count() * staging_->page_size()) >> 20,
                      (Submissions::kSlots * Submissions::kUniformBytes) >> 20);
  reported_ = s;
  reported_textures_ = t;
  reported_resolves_ = v;
  reported_submissions_ = submissions;
  return line;
}

}  // namespace nier::native
