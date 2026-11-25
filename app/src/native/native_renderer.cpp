#include "native_renderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>
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
#include "vk_recorder.h"

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

REXCVAR_DEFINE_INT32(nier_readback_limit_kb, 16, "NieR",
                     "Native renderer: largest resolve, in KB, that is also written back to guest memory");
REXCVAR_DEFINE_BOOL(nier_mirror_staging, true, "NieR",
                    "Native renderer: copy changed vertex/index ranges still read by the GPU through a GPU copy "
                    "instead of waiting for the GPU");
REXCVAR_DEFINE_BOOL(nier_draw_state_filter, true, "NieR",
                    "Native renderer: skip dynamic state, descriptor bindings and constant uploads that repeat the "
                    "previous draw");
REXCVAR_DEFINE_BOOL(nier_fsr, true, "NieR",
                    "Native renderer: upscale the reduced-resolution scene to the 1280-wide composite with FSR 1 (EASU) "
                    "instead of the game's bilinear stretch");
REXCVAR_DEFINE_DOUBLE(nier_fsr_sharpness, 0.25, "NieR",
                      "Native renderer: FSR 1 RCAS sharpening after the upscale, in stops of reduction (0 = sharpest, "
                      "2 = soft); negative skips RCAS");
REXCVAR_DEFINE_DOUBLE(nier_fsr_gpu_budget_ms, 30.0, "NieR",
                      "Native renderer: GPU time per frame above which quality steps down (FSR sharpening, then the "
                      "scene resolution, then the shadow map resolution, then the upscale) when the GPU is busy for "
                      "most of the frame; it steps back up after two seconds with room for it");
REXCVAR_DEFINE_BOOL(nier_adaptive_shadows, true, "NieR",
                    "Native renderer: let the GPU governor render the shadow map at half resolution as its last step");
REXCVAR_DEFINE_BOOL(nier_draw_memo, true, "NieR",
                    "Native renderer: when only constants, vertices and indices changed since the previous draw, reuse "
                    "its shaders, textures, descriptor sets and pipeline instead of preparing them again");
REXCVAR_DEFINE_BOOL(nier_draw_memo_verify, false, "NieR",
                    "Native renderer: on every reused draw also prepare it again and log '[native] draw memo "
                    "DIFFERENCE' when the results differ");
REXCVAR_DEFINE_BOOL(nier_readback_poll, false, "NieR",
                    "Native renderer: while resolves read back to guest memory are in flight, check every 16 draws "
                    "whether the GPU finished them, so the game (auto exposure) gets them sooner");
REXCVAR_DEFINE_BOOL(nier_readback_submit, true, "NieR",
                    "Native renderer: submit the commands right after the frame's last resolve read back to guest "
                    "memory (learnt from the previous frame) instead of at the end of the frame");
REXCVAR_DEFINE_BOOL(nier_early_depth_tests, true, "NieR",
                    "Native renderer: run depth/stencil tests before pixel shaders that kill pixels when the draw "
                    "writes neither depth nor stencil (same result, hidden pixels are not shaded)");
REXCVAR_DEFINE_BOOL(nier_store_cards, true, "NieR",
                    "Native renderer: find changed guest memory from store cards instead of hashing it (needs code "
                    "generated with REX_STORE_CARDS)");
REXCVAR_DEFINE_BOOL(nier_store_cards_verify, false, "NieR",
                    "Native renderer: compare the mirror and textures with guest memory where store cards say nothing "
                    "changed, and count misses (slow, for checking)");
REXCVAR_DEFINE_BOOL(nier_record_thread, true, "NieR",
                    "Native renderer: Vulkan commands are recorded on a worker thread from a queue the ring fills");
REXCVAR_DEFINE_BOOL(nier_vertex_specialization, true, "NieR",
                    "Native renderer: vertex shaders get the texture constants and flags of the draw baked in, like "
                    "pixel shaders");
REXCVAR_DEFINE_BOOL(nier_constant_generations_verify, false, "NieR",
                    "Native renderer: pack float constants even when their registers did not change and count the "
                    "draws whose reused constants differ (slow, for checking)");
REXCVAR_DEFINE_BOOL(nier_skip_silent_draws, true, "NieR",
                    "Native renderer: skip draws that write no color, depth, stencil or memory");
REXCVAR_DEFINE_BOOL(nier_gpu_shader_marks, false, "NieR",
                    "Native renderer: GPU timestamps at every pixel shader change (per-shader GPU times in the report)");
REXCVAR_DEFINE_INT32(nier_shadow_map_shift, 0, "NieR",
                     "Native renderer: the shadow map is drawn at 1/2^N of its size (0 = full size)");
REXCVAR_DEFINE_INT32(nier_shadow_mask_shift, 1, "NieR",
                     "Native renderer: the screen-space shadow mask is drawn at 1/2^N of its size (0 = full size)");

REXCVAR_DECLARE(bool, nier_dynamic_resolution);

namespace nier::native {

using namespace rex::graphics;
namespace vk = rex::ui::vulkan;
using Translator = SpirvShaderTranslator;

namespace {

bool UpscalableFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
      return true;
    default:
      return false;
  }
}

#if REX_PLATFORM_SWITCH
uint64_t ReadTicks() {
  uint64_t ticks;
  asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
  return ticks;
}

double MicrosPerTick() {
  uint64_t frequency;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  return 1e6 / double(frequency);
}
#else
uint64_t ReadTicks() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

double MicrosPerTick() { return 1e-3; }
#endif

const double kMicrosPerTick = MicrosPerTick();

class ScopedMicros {
 public:
  explicit ScopedMicros(uint64_t& total) : total_(total), start_(ReadTicks()) {}
  ~ScopedMicros() { total_ += uint64_t(double(ReadTicks() - start_) * kMicrosPerTick); }

 private:
  uint64_t& total_;
  uint64_t start_;
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
constexpr uint64_t kUnusedTextureSeconds = 60;
constexpr size_t kReportedPasses = 8;
constexpr size_t kReportedShaders = 10;
constexpr size_t kTracedFrames = 12;
constexpr uint64_t kTracedSwaps = 3000;
constexpr uint32_t kGovernorFrames = 15;

QualityOptions CurrentQualityOptions() {
  return {REXCVAR_GET(nier_fsr), REXCVAR_GET(nier_fsr_sharpness) >= 0.0, REXCVAR_GET(nier_dynamic_resolution),
          REXCVAR_GET(nier_adaptive_shadows)};
}
constexpr uint32_t kReadbackPollDraws = 16;

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
      card_table_(memory->TranslateVirtual<uint8_t*>(0)),
      shaders_(shaders),
      registers_(registers),
      extents_(registers.file(), *memory) {
  if (!device_->properties().dynamicRendering) {
    REXLOG_ERROR("[native] the device has no dynamic rendering, nothing will be drawn");
    return;
  }
  recorder_ = std::make_unique<VulkanRecorder>(device_->functions());
  const auto& dfn = Vk(device_);
  const VkDevice d = device_->device();
  pipeline_report_ = std::make_unique<PipelineReport>(device_);
  pipelines_ = std::make_unique<Pipelines>(device_, *pipeline_report_);
  const bool cached_uploads = PreferCachedUploads(device_);
  submissions_ = std::make_unique<Submissions>(device_, pipelines_->ConstantsLayout(), cached_uploads);
  mirror_ = std::make_unique<GuestMirror>(device_, physical_base_, cached_uploads);
  if (!mirror_->valid()) {
    return;
  }
  targets_ = std::make_unique<RenderTargets>(device_, *submissions_);
  staging_ = std::make_unique<vk::VulkanUploadBufferPool>(device_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kStagingPageBytes,
                                                         cached_uploads);
  textures_ = std::make_unique<Textures>(device_, physical_base_, *staging_);
  resolves_ = std::make_unique<Resolves>(device_, *targets_, *textures_, memory_, physical_base_);
  upscale_ = std::make_unique<SceneUpscale>(device_, *submissions_);
  pipeline_report_->Report(resolves_->depth_pipeline(), "depth_to_float");
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
  gpu_governor_.Configure(CurrentQualityOptions(), REXCVAR_GET(nier_fsr_gpu_budget_ms));
  g_scene_reduced.store(false, std::memory_order_relaxed);
  valid_ = true;
  REXLOG_INFO("[native] renderer ready: {} shared memory bindings, depth {}", shared_count,
              targets_->depth_format() == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
}

void NativeRenderer::CreateGammaPipeline() {
  const auto& dfn = Vk(device_);
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
  const auto& dfn = Vk(device_);
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
  SetActiveRecorder(recorder_ && recorder_->running() && REXCVAR_GET(nier_record_thread) ? recorder_.get() : nullptr);
  {
    ScopedMicros timer(stats_.us_gpu_wait);
    submissions_->Begin();
  }
  if (mirror_cards_) {
    mirror_cards_->NextInterval();
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

bool NativeRenderer::CardsEnabled() const {
#if defined(REX_STORE_CARDS)
  return REXCVAR_GET(nier_store_cards);
#else
  return false;
#endif
}

void NativeRenderer::SyncMirror(uint32_t address, uint32_t size) {
  if (!size || address >= GuestMirror::kSize) {
    return;
  }
  size = std::min(size, GuestMirror::kSize - address);
  if (cards_mode_) {
    SyncMirrorCards(address, size);
    return;
  }
  ScopedMicros timer(stats_.us_mirror);
  const MirrorDecision decision = mirror_sync_.Use(address, size, physical_base_, submissions_->current());
  switch (RouteMirrorCopy(decision, submissions_->current(), size, uint32_t(kStagingPageBytes),
                          mirror_->gpu_writable() && REXCVAR_GET(nier_mirror_staging))) {
    case MirrorRoute::kNone:
      return;
    case MirrorRoute::kInPlace:
      CopyMirror(address, size);
      return;
    case MirrorRoute::kFlushThenStage:
      ++stats_.mirror_flushes;
      Flush();
      BeginSubmission();
      mirror_sync_.Use(address, size, physical_base_, submissions_->current());
      [[fallthrough]];
    case MirrorRoute::kStage:
      if (mirror_->Stage(submissions_->upload(), submissions_->current(), *staging_, address, size)) {
        ++stats_.mirror_staged;
        stats_.mirror_staged_bytes += size;
        return;
      }
      [[fallthrough]];
    case MirrorRoute::kWait:
      ++stats_.mirror_waits;
      WaitFor(decision.wait_submission);
      BeginSubmission();
      mirror_sync_.Use(address, size, physical_base_, submissions_->current());
      CopyMirror(address, size);
      return;
  }
}

void NativeRenderer::SyncMirrorCards(uint32_t address, uint32_t size) {
  ScopedMicros timer(stats_.us_mirror);
  card_runs_.clear();
  mirror_cards_->Take(address, size, card_runs_);
  for (const PageRun& run : card_runs_) {
    CopyMirrorRun(run);
  }
  mirror_readers_.Record(address, size, submissions_->current());
  if (REXCVAR_GET(nier_store_cards_verify)) {
    VerifyMirror(address, size);
  }
}

void NativeRenderer::CopyMirrorRun(const PageRun& run) {
  const uint32_t address = run.first << kCardPageShift;
  const uint32_t size = std::min(run.count << kCardPageShift, GuestMirror::kSize - address);
  stats_.cards_copied_bytes += size;
  const uint64_t wait = mirror_readers_.Pending(run.first, run.count, submissions_->completed());
  const bool can_stage =
      mirror_->gpu_writable() && REXCVAR_GET(nier_mirror_staging) && !REXCVAR_GET(nier_store_cards_verify);
  switch (RouteMirrorCopy({MirrorAction::kCopy, wait}, submissions_->current(), size, uint32_t(kStagingPageBytes),
                          can_stage)) {
    case MirrorRoute::kNone:
      return;
    case MirrorRoute::kInPlace:
      CopyMirror(address, size);
      return;
    case MirrorRoute::kFlushThenStage:
      ++stats_.mirror_flushes;
      Flush();
      BeginSubmission();
      [[fallthrough]];
    case MirrorRoute::kStage:
      if (mirror_->Stage(submissions_->upload(), submissions_->current(), *staging_, address, size)) {
        ++stats_.mirror_staged;
        stats_.mirror_staged_bytes += size;
        return;
      }
      [[fallthrough]];
    case MirrorRoute::kWait:
      ++stats_.mirror_waits;
      WaitFor(wait);
      BeginSubmission();
      CopyMirror(address, size);
      return;
  }
}

void NativeRenderer::VerifyMirror(uint32_t address, uint32_t size) {
  const uint8_t* guest = physical_base_ + address;
  const uint8_t* mirrored = mirror_->data() + address;
  for (uint32_t offset = 0; offset < size;) {
    const uint32_t page = (address + offset) >> kCardPageShift;
    const uint32_t chunk = std::min(size - offset, ((page + 1) << kCardPageShift) - (address + offset));
    if (std::memcmp(guest + offset, mirrored + offset, chunk) != 0 && mirror_cards_->Clean(page, 1)) {
      ++stats_.cards_missed;
      if (reported_cards_misses_ < 32) {
        ++reported_cards_misses_;
        REXLOG_WARN("[native] store cards missed a change: range {:08X}+{:X}, page {:08X}", address, size,
                    page << kCardPageShift);
      }
    }
    offset += chunk;
  }
}

void NativeRenderer::CopyMirror(uint32_t address, uint32_t size) {
  mirror_->Copy(address, size);
  ++stats_.mirror_copies;
  stats_.mirror_bytes += size;
}

VkDescriptorSet NativeRenderer::BindTextures(const RegisterFile& regs, const SpirvShader& shader, bool pixel,
                                             TextureConstantInfo& info, uint32_t& textures, uint32_t& samplers,
                                             bool& out_of_memory) {
  const auto& texture_bindings = shader.GetTextureBindingsAfterTranslation();
  const auto& sampler_bindings = shader.GetSamplerBindingsAfterTranslation();
  textures = uint32_t(texture_bindings.size());
  samplers = uint32_t(sampler_bindings.size());
  const VkDescriptorSetLayout layout = pipelines_->TextureSetLayout(pixel, textures, samplers);
  std::vector<VkDescriptorImageInfo>& images = bind_images_;
  std::vector<uint64_t>& handles = bind_handles_;
  images.resize(textures + samplers);
  handles.resize(textures + samplers + 1);
  handles[0] = uint64_t(layout);
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
    if (bound.mip_chain_stale) {
      const VkCommandBuffer cmd = submissions_->main();
      targets_->End(cmd);
      textures_->UpdateMipChain(cmd, bound.mip_chain);
    }
    if (bound.resolve_address && upscale_target_width_) {
      const SceneUpscalePlan plan = PlanSceneUpscale(bound.resolve_width, bound.resolve_height, upscale_target_width_);
      if (plan.wanted && UpscalableFormat(bound.resolve_format)) {
        bound.view = UpscaledScene(bound, plan);
      }
    }
    if (bound.resolve_address && Tracing()) {
      trace_pass_.samples.insert({bound.resolve_address, (fetch.size_2d.width + 1) << 16 | (fetch.size_2d.height + 1)});
    }
    images[i] = {VK_NULL_HANDLE, bound.view, VK_IMAGE_LAYOUT_GENERAL};
    handles[i + 1] = uint64_t(bound.view);
    info.used_mask |= 1u << binding.fetch_constant;
    info.swizzled_signs[binding.fetch_constant] = bound.swizzled_signs;
    info.host_swizzles[binding.fetch_constant] = bound.host_swizzle;
  }
  for (uint32_t i = 0; i < samplers; ++i) {
    const auto& binding = sampler_bindings[i];
    const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
    images[textures + i] = {textures_->Sampler(fetch, binding), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    handles[textures + i + 1] = uint64_t(images[textures + i].sampler);
  }
  bool fresh = PerfSkip(kFreshDescriptors);
  const VkDescriptorSet set =
      fresh ? submissions_->Allocate(layout)
            : submissions_->AllocateShared(layout, XXH3_64bits(handles.data(), handles.size() * sizeof(uint64_t)),
                                           fresh);
  if (set == VK_NULL_HANDLE || !fresh || images.empty()) {
    return set;
  }
  std::vector<VkWriteDescriptorSet>& writes = bind_writes_;
  writes.resize(images.size());
  for (uint32_t i = 0; i < writes.size(); ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 set,
                 i,
                 0,
                 1,
                 i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                 &images[i],
                 nullptr,
                 nullptr};
  }
  Vk(device_).vkUpdateDescriptorSets(device_->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  return set;
}

VkImageView NativeRenderer::UpscaledScene(const Textures::Binding& bound, const SceneUpscalePlan& plan) {
  const Quality& quality = gpu_governor_.quality();
  if (!quality.upscale) {
    return bound.view;
  }
  const float sharpness = quality.sharpen ? float(REXCVAR_GET(nier_fsr_sharpness)) : -1.0f;
  if (const VkImageView ready =
          upscale_->Ready(bound.view, bound.resolve_format, plan.width, plan.height, bound.resolve_version, sharpness)) {
    return ready;
  }
  const VkCommandBuffer cmd = submissions_->main();
  targets_->End(cmd);
  submissions_->Mark(GpuCategory::kResolves,
                     LabelId(NamedPass(fmt::format("fsr {}x{} -> {}x{}", bound.resolve_width, bound.resolve_height,
                                                   plan.width, plan.height),
                                       true),
                             nullptr, nullptr));
  const VkImageView view =
      upscale_->Upscale(cmd, bound.view, bound.resolve_plain_view, bound.resolve_format, bound.resolve_width,
                        bound.resolve_height, plan.width, plan.height, bound.resolve_version, sharpness);
  bound_pipeline_ = VK_NULL_HANDLE;
  dynamic_filter_.Reset();
  binding_filter_.Reset();
  return view;
}

VkDescriptorSet NativeRenderer::EmptyTextureSet() {
  const VkDescriptorSetLayout layout = pipelines_->TextureSetLayout(true, 0, 0);
  bool fresh = false;
  return submissions_->AllocateShared(layout, XXH3_64bits(&layout, sizeof(layout)), fresh);
}

uint32_t NativeRenderer::DownscaleFor(const TargetSet& targets, bool writes_depth) {
  if (scaled_shift_ && ((targets.depth && targets.depth == scaled_depth_) ||
                        (targets.color[0] && targets.color[0] == scaled_color_))) {
    return scaled_shift_;
  }
  uint32_t shift = 0;
  switch (KindOf(targets)) {
    case PassKind::kShadowMap:
      shift = writes_depth ? uint32_t(std::clamp(std::max(REXCVAR_GET(nier_shadow_map_shift),
                                                          gpu_governor_.quality().full_shadows ? 0 : 1),
                                                 0, 3))
                           : 0;
      break;
    case PassKind::kScreen640:
      shift = after_shadow_map_ ? uint32_t(std::clamp(REXCVAR_GET(nier_shadow_mask_shift), 0, 3)) : 0;
      break;
    default:
      break;
  }
  after_shadow_map_ = false;
  if (shift) {
    scaled_shift_ = shift;
    scaled_color_ = targets.color[0];
    scaled_depth_ = targets.depth;
  }
  return shift;
}

VkRect2D NativeRenderer::SetDynamicState(const RegisterFile& regs, const draw_util::ViewportInfo& viewport_info,
                                     const SampleScale& scale, bool polygonal, uint32_t shift) {
  const auto& dfn = Vk(device_);
  const VkCommandBuffer cmd = submissions_->main();
  VkViewport viewport{-1.0f, -1.0f, 1.0f, 1.0f, viewport_info.z_min, viewport_info.z_max};
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    const float factor = 1.0f / float(1u << shift);
    viewport.x = float(viewport_info.xy_offset[0]) * factor;
    viewport.y = float(viewport_info.xy_offset[1]) * factor;
    viewport.width = float(viewport_info.xy_extent[0]) * factor;
    viewport.height = float(viewport_info.xy_extent[1]) * factor;
  }
  DynamicState next;
  next.viewport = viewport;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const VkExtent2D area = targets_->render_area();
  const ScaledSpan columns = ScaledRange(scissor.offset[0] * scale.x, scissor.extent[0] * scale.x, shift, area.width);
  const ScaledSpan rows = ScaledRange(scissor.offset[1] * scale.y, scissor.extent[1] * scale.y, shift, area.height);
  const VkRect2D rect{{int32_t(columns.begin), int32_t(rows.begin)}, {columns.length, rows.length}};
  VkRect2D host_rect = rect;
  if (PerfSkip(kSkipPixels)) {
    host_rect.extent = {0, 0};
  } else if (PerfSkip(kQuarterScreen)) {
    host_rect.extent = {rect.extent.width / 2, rect.extent.height / 2};
  }
  next.scissor = host_rect;
  float slope = 0.0f, constant = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(regs, polygonal, slope, constant);
  constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
                  ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                  : draw_util::kD3D10PolygonOffsetFactorFloat24;
  slope *= xenos::kPolygonOffsetScaleSubpixelUnit * float(std::max(scale.x, scale.y));
  next.depth_bias_constant = constant;
  next.depth_bias_slope = slope;
  next.blend = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  const auto depth = draw_util::GetNormalizedDepthControl(regs);
  auto front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
  auto back = front;
  if (polygonal && depth.backface_enable) {
    back = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  }
  next.compare_mask = {front.stencilmask, back.stencilmask};
  next.write_mask = {front.stencilwritemask, back.stencilwritemask};
  next.reference = {front.stencilref, back.stencilref};
  const uint32_t changed = REXCVAR_GET(nier_draw_state_filter) ? dynamic_filter_.Changed(next) : kAllDynamicState;
  const uint32_t calls = uint32_t(std::popcount(changed));
  stats_.dynamic_calls += calls;
  stats_.dynamic_skipped += 10 - calls;
  if (changed & kViewportBit) {
    dfn.vkCmdSetViewport(cmd, 0, 1, &next.viewport);
  }
  if (changed & kScissorBit) {
    dfn.vkCmdSetScissor(cmd, 0, 1, &next.scissor);
  }
  if (changed & kDepthBiasBit) {
    dfn.vkCmdSetDepthBias(cmd, next.depth_bias_constant, 0.0f, next.depth_bias_slope);
  }
  if (changed & kBlendBit) {
    dfn.vkCmdSetBlendConstants(cmd, next.blend.data());
  }
  if (changed & kCompareMaskFrontBit) {
    dfn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, next.compare_mask[0]);
  }
  if (changed & kCompareMaskBackBit) {
    dfn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, next.compare_mask[1]);
  }
  if (changed & kWriteMaskFrontBit) {
    dfn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, next.write_mask[0]);
  }
  if (changed & kWriteMaskBackBit) {
    dfn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, next.write_mask[1]);
  }
  if (changed & kReferenceFrontBit) {
    dfn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, next.reference[0]);
  }
  if (changed & kReferenceBackBit) {
    dfn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, next.reference[1]);
  }
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
  if (REXCVAR_GET(nier_readback_poll) && resolves_->readbacks_pending() && ++readback_poll_draws_ >= kReadbackPollDraws) {
    readback_poll_draws_ = 0;
    ProcessCompleted();
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
  if (REXCVAR_GET(nier_draw_memo) && MemoMatches(regs, draw)) {
    BeginSubmission();
    const uint64_t submission = submissions_->current();
    if (submissions_->HasUniformSpace(kDrawUniformBytes)) {
      SyncDrawRanges(regs, draw, *memo_.prepared.vertex_shader);
      if (submissions_->current() == submission) {
        ++stats_.memo_hits;
        if (!REXCVAR_GET(nier_draw_memo_verify)) {
          return RecordPrepared(regs, draw, memo_, false);
        }
        VerifyMemo(regs, draw, polygonal);
        if (submissions_->current() == submission) {
          return RecordPrepared(regs, draw, memo_, true);
        }
      }
    }
  }
  memo_.valid = false;
  DrawMemo next;
  if (const auto result = PrepareState(regs, draw, polygonal, next)) {
    return *result;
  }
  memo_ = next;
  return RecordPrepared(regs, draw, memo_, true);
}

bool NativeRenderer::MemoMatches(const RegisterFile& regs, const HostDraw& draw) const {
  const DrawMemo& m = memo_;
  if (!m.valid || !registers_.StateUnchanged() || m.foreign_state_writes != registers_.foreign_state_writes() ||
      m.texture_bindings != textures_->binding_version() || m.submission != submissions_->current() ||
      m.swaps != stats_.swaps || !shaders_.Current(m.vertex_hash, m.pixel_hash) || m.topology != draw.topology ||
      m.primitive_restart != draw.primitive_restart || m.vs_type != draw.vs_type ||
      m.index_source != draw.index_source || m.index_32bit != draw.index_32bit ||
      m.index_endian != draw.index_endian || Tracing()) {
    return false;
  }
  for (uint32_t mask = m.fetch_mask; mask; mask &= mask - 1) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    if (std::memcmp(&regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6], &m.fetch_words[slot * 6],
                    6 * sizeof(uint32_t)) != 0) {
      return false;
    }
  }
  return true;
}

void NativeRenderer::SyncDrawRanges(const RegisterFile& regs, const HostDraw& draw,
                                    const vulkan::VulkanShader& vertex_shader) {
  auto sync_ranges = [&] {
    std::array<std::pair<uint32_t, uint32_t>, 8> synced;
    size_t synced_count = 0;
    for (const auto& binding : vertex_shader.vertex_bindings()) {
      const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
      const std::pair<uint32_t, uint32_t> range{fetch.address << 2, fetch.size << 2};
      if (std::find(synced.begin(), synced.begin() + synced_count, range) != synced.begin() + synced_count) {
        continue;
      }
      if (synced_count < synced.size()) {
        synced[synced_count++] = range;
      }
      SyncMirror(range.first, range.second);
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
}

std::optional<NativeRenderer::DrawResult> NativeRenderer::PrepareState(const RegisterFile& regs, const HostDraw& draw,
                                                                       bool polygonal, DrawMemo& out) {
  const SampleScale scale = ScaleFor(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
  bool shader_pending = false;
  auto prepared = shaders_.PrepareDraw(regs, draw.vs_type, scale.x, scale.y, 0, shader_pending);
  if (!prepared) {
    ++(shader_pending ? stats_.skipped_pending : stats_.skipped_shader);
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
  if (REXCVAR_GET(nier_skip_silent_draws) && !prepared->vertex_shader->memexport_eM_written() &&
      !(prepared->pixel_shader && prepared->pixel_shader->memexport_eM_written()) &&
      DrawWritesNothing(color_mask, draw_util::GetNormalizedDepthControl(regs),
                        regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK),
                        regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF))) {
    ++stats_.skipped_silent;
    return DrawResult::kDone;
  }
  BeginSubmission();
  if (!submissions_->HasUniformSpace(kDrawUniformBytes)) {
    Flush();
    BeginSubmission();
  }
  SyncDrawRanges(regs, draw, *prepared->vertex_shader);
  TextureConstantInfo texture_info;
  uint32_t vs_textures = 0, vs_samplers = 0, ps_textures = 0, ps_samplers = 0;
  bool out_of_memory = false;
  upscale_target_width_ =
      REXCVAR_GET(nier_fsr) && targets.color[0] && !targets.depth ? ExtentFor(*targets.color[0]).width : 0;
  const uint64_t texture_waits = textures_->stats().background_waits;
  const uint64_t mip_chain_binds = textures_->stats().mip_chain_binds;
  const VkDescriptorSet vs_set =
      BindTextures(regs, *prepared->vertex_shader, false, texture_info, vs_textures, vs_samplers, out_of_memory);
  if (vs_set == VK_NULL_HANDLE) {
    return out_of_memory ? DrawResult::kOutOfMemory : DrawResult::kRetry;
  }
  const VkDescriptorSet ps_set =
      prepared->pixel_shader
          ? BindTextures(regs, *prepared->pixel_shader, true, texture_info, ps_textures, ps_samplers, out_of_memory)
          : EmptyTextureSet();
  if (ps_set == VK_NULL_HANDLE) {
    return out_of_memory ? DrawResult::kOutOfMemory : DrawResult::kRetry;
  }
  const uint32_t shift =
      DownscaleFor(targets, prepared->pixel_shader && prepared->pixel_shader->writes_depth());
  if (shift && prepared->pixel_position) {
    prepared = shaders_.PrepareDraw(regs, draw.vs_type, scale.x, scale.y, shift, shader_pending);
    if (!prepared) {
      ++(shader_pending ? stats_.skipped_pending : stats_.skipped_shader);
      return DrawResult::kDone;
    }
  }
  const auto& properties = device_->properties();
  draw_util::ViewportInfo viewport;
  draw_util::GetHostViewportInfo(regs, targets.scale.x, targets.scale.y, false, properties.maxViewportDimensions[0],
                                 properties.maxViewportDimensions[1], true, draw_util::GetNormalizedDepthControl(regs),
                                 false, true, prepared->pixel_shader && prepared->pixel_shader->writes_depth(),
                                 viewport);
  Translator::SystemConstants system;
  FillSystemConstants(regs, draw, viewport, texture_info, system);
  uint32_t fetches[192];
  CopyFetchConstants(regs, fetches);
  PipelineState state = BuildPipelineState(regs, draw, targets, color_mask, targets_->depth_format());
  state.vertex_module = REXCVAR_GET(nier_vertex_specialization) && !PerfSkip(kGenericShaders)
                            ? shaders_.VertexModule(*prepared->vertex_shader, *prepared->vertex, system, fetches)
                            : prepared->vertex->shader_module();
  if (prepared->pixel) {
    const bool early_tests =
        REXCVAR_GET(nier_early_depth_tests) && !prepared->pixel_shader->memexport_eM_written() &&
        EarlyTestsHelp(prepared->pixel_shader->kills_pixels(), prepared->pixel_shader->writes_depth(),
                       draw_util::GetNormalizedDepthControl(regs),
                       regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK),
                       regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF));
    state.pixel_module = PerfSkip(kGenericShaders) ? prepared->pixel->shader_module()
                                                   : shaders_.PixelModule(*prepared->pixel, system, fetches, early_tests);
  }
  if (PerfSkip(kSkipPixelShaders)) {
    state.pixel_module = VK_NULL_HANDLE;
  }
  const VkPipelineLayout layout = pipelines_->Layout(vs_textures, vs_samplers, ps_textures, ps_samplers);
  Pipelines::Status pipeline_status;
  const VkPipeline pipeline =
      pipelines_->Get(state, layout, prepared->vertex_shader->ucode_data_hash(),
                      prepared->pixel_shader ? prepared->pixel_shader->ucode_data_hash() | 1 : 0, pipeline_status);
  if (pipeline_status == Pipelines::Status::kPending) {
    ++stats_.skipped_pending;
    return DrawResult::kDone;
  }
  if (pipeline == VK_NULL_HANDLE) {
    ++stats_.skipped_shader;
    return DrawResult::kDone;
  }
  out.prepared = *prepared;
  out.color_mask = color_mask;
  out.targets = targets;
  out.vs_set = vs_set;
  out.ps_set = ps_set;
  out.texture_info = texture_info;
  out.shift = shift;
  out.viewport = viewport;
  out.state = state;
  out.layout = layout;
  out.pipeline = pipeline;
  out.polygonal = polygonal;
  out.topology = draw.topology;
  out.primitive_restart = draw.primitive_restart;
  out.vs_type = draw.vs_type;
  out.index_source = draw.index_source;
  out.index_32bit = draw.index_32bit;
  out.index_endian = draw.index_endian;
  out.fetch_mask = UsedFetchConstants(*prepared);
  std::memcpy(out.fetch_words.data(), &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(out.fetch_words));
  shaders_.CurrentHashes(out.vertex_hash, out.pixel_hash);
  out.foreign_state_writes = registers_.foreign_state_writes();
  out.texture_bindings = textures_->binding_version();
  registers_.BeginStateJournal();
  out.submission = submissions_->current();
  out.swaps = stats_.swaps;
  out.valid = !Tracing() && textures_->stats().background_waits == texture_waits &&
              textures_->stats().mip_chain_binds == mip_chain_binds;
  return std::nullopt;
}

uint32_t NativeRenderer::UsedFetchConstants(const NativeShaders::PreparedDraw& prepared) {
  uint32_t mask = 0;
  for (const vulkan::VulkanShader* shader : {prepared.vertex_shader, prepared.pixel_shader}) {
    if (!shader) {
      continue;
    }
    for (const auto& binding : shader->GetTextureBindingsAfterTranslation()) {
      mask |= 1u << binding.fetch_constant;
    }
    for (const auto& binding : shader->GetSamplerBindingsAfterTranslation()) {
      mask |= 1u << binding.fetch_constant;
    }
  }
  return mask;
}

void NativeRenderer::VerifyMemo(const RegisterFile& regs, const HostDraw& draw, bool polygonal) {
  DrawMemo fresh;
  const uint64_t submission = submissions_->current();
  if (PrepareState(regs, draw, polygonal, fresh) || submissions_->current() != submission) {
    return;
  }
  const DrawMemo& m = memo_;
  std::string differences;
  auto check = [&](bool same, const char* what) {
    if (!same) {
      differences += differences.empty() ? what : fmt::format(", {}", what);
    }
  };
  check(fresh.prepared.vertex == m.prepared.vertex && fresh.prepared.pixel == m.prepared.pixel &&
            fresh.prepared.vertex_shader == m.prepared.vertex_shader &&
            fresh.prepared.pixel_shader == m.prepared.pixel_shader,
        "shaders");
  check(fresh.color_mask == m.color_mask && fresh.targets == m.targets, "targets");
  check(fresh.vs_set == m.vs_set && fresh.ps_set == m.ps_set, "texture sets");
  check(std::memcmp(&fresh.texture_info, &m.texture_info, sizeof(m.texture_info)) == 0, "texture constants");
  check(fresh.shift == m.shift, "shift");
  check(std::memcmp(&fresh.viewport, &m.viewport, sizeof(m.viewport)) == 0, "viewport");
  check(fresh.state == m.state, "pipeline state");
  check(fresh.layout == m.layout && fresh.pipeline == m.pipeline, "pipeline");
  if (differences.empty()) {
    return;
  }
  if (++stats_.memo_mismatches <= 16) {
    REXLOG_WARN("[native] draw memo DIFFERENCE: {}", differences);
  }
}

NativeRenderer::DrawResult NativeRenderer::RecordPrepared(const RegisterFile& regs, const HostDraw& draw,
                                                          const DrawMemo& m, bool set_dynamic_state) {
  Translator::SystemConstants system;
  FillSystemConstants(regs, draw, m.viewport, m.texture_info, system);
  alignas(16) uint8_t floats[Submissions::kFloatConstantsBytes];
  uint32_t bools[40];
  std::array<uint32_t, Translator::kConstantBufferCount> offsets{};
  const bool filter = REXCVAR_GET(nier_draw_state_filter);
  auto upload = [&](uint32_t stream, const void* data, size_t size, size_t reserve) {
    if (!filter) {
      submissions_->Uniform(data, size, reserve, offsets[stream]);
      ++stats_.uniforms_written;
      return;
    }
    bool reused = false;
    submissions_->RepeatedUniform(stream, data, size, reserve, offsets[stream], reused);
    ++(reused ? stats_.uniforms_reused : stats_.uniforms_written);
  };
  upload(Translator::kConstantBufferSystem, &system, sizeof(system), sizeof(system));
  const bool verify_floats = REXCVAR_GET(nier_constant_generations_verify);
  auto upload_floats = [&](uint32_t stream, const Shader::ConstantRegisterMap* map, bool pixel) {
    FloatUpload& cached = float_uploads_[pixel];
    const uint64_t generation = map ? registers_.FloatGeneration(map->float_bitmap, pixel) : 0;
    if (filter && map && cached.map == map && cached.generation == generation &&
        cached.submission == submissions_->current()) {
      offsets[stream] = cached.offset;
      ++stats_.uniforms_reused;
      ++stats_.uniforms_unpacked;
      if (verify_floats) {
        const uint32_t size = PackFloatConstants(regs, *map, pixel, floats);
        stats_.uniforms_stale += XXH3_64bits(floats, size) != cached.hash;
      }
      return;
    }
    const uint32_t size = PackFloatConstants(regs, map ? *map : Shader::ConstantRegisterMap{}, pixel, floats);
    upload(stream, floats, size, Submissions::kFloatConstantsBytes);
    cached = {map, generation, submissions_->current(), offsets[stream], verify_floats ? XXH3_64bits(floats, size) : 0};
  };
  upload_floats(Translator::kConstantBufferFloatVertex, &m.prepared.vertex_shader->constant_register_map(), false);
  upload_floats(Translator::kConstantBufferFloatPixel,
                m.prepared.pixel_shader ? &m.prepared.pixel_shader->constant_register_map() : nullptr, true);
  auto upload_range = [&](uint32_t stream, uint32_t first, uint32_t count, RangeUpload& cached, auto&& fill,
                          void* data, size_t size) {
    const uint64_t generation = registers_.ConstantsGeneration(first, count);
    if (filter && cached.valid && cached.generation == generation && cached.submission == submissions_->current()) {
      offsets[stream] = cached.offset;
      ++stats_.uniforms_reused;
      if (verify_floats) {
        fill();
        stats_.uniforms_stale += XXH3_64bits(data, size) != cached.hash;
      }
      return;
    }
    fill();
    upload(stream, data, size, size);
    cached = {generation, submissions_->current(), offsets[stream], verify_floats ? XXH3_64bits(data, size) : 0, true};
  };
  upload_range(Translator::kConstantBufferBoolLoop, XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031, 40, bool_upload_,
               [&] { CopyBoolLoopConstants(regs, bools); }, bools, sizeof(bools));
  uint32_t fetches[192];
  upload_range(Translator::kConstantBufferFetch, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 192, fetch_upload_,
               [&] { CopyFetchConstants(regs, fetches); }, fetches, sizeof(fetches));
  const VkCommandBuffer cmd = submissions_->main();
  if (targets_->Begins(m.targets)) {
    targets_->End(cmd);
    current_pass_ = PassId(m.targets);
    submissions_->Mark(GpuCategory::kDraws, LabelId(current_pass_, nullptr, nullptr));
    FlushTracePass();
    trace_pass_.pass = current_pass_;
    if (Tracing()) {
      frame_passes_.push_back(current_pass_);
    }
    set_dynamic_state = true;
  }
  targets_->Begin(cmd, m.targets,
                  UsedRows(m.targets, extents_.EstimateMaxY(true, *m.prepared.vertex_shader) * m.targets.scale.y),
                  m.shift, KindOf(m.targets) == PassKind::kShadowMap);
  if (!targets_->rendering()) {
    ++stats_.skipped_target;
    memo_.valid = false;
    return DrawResult::kDone;
  }
  const auto& dfn = Vk(device_);
  if (submissions_->current() != filter_submission_) {
    dynamic_filter_.Reset();
    binding_filter_.Reset();
    filter_submission_ = submissions_->current();
    set_dynamic_state = true;
  }
  if (m.pipeline != bound_pipeline_ || submissions_->current() != bound_submission_ || PerfSkip(kRebindPipelines)) {
    dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipeline);
    bound_pipeline_ = m.pipeline;
    bound_submission_ = submissions_->current();
  }
  submissions_->Mark(GpuCategory::kDraws,
                     REXCVAR_GET(nier_gpu_shader_marks)
                         ? LabelId(current_pass_, m.prepared.pixel_shader, m.prepared.pixel)
                         : LabelId(current_pass_, nullptr, nullptr));
  const DescriptorBinding binding{m.layout, {shared_set_, submissions_->constants_set(), m.vs_set, m.ps_set}, offsets};
  const uint32_t first_set = filter ? binding_filter_.FirstChanged(binding) : 0;
  if (first_set < kBoundSets) {
    ++stats_.set_binds;
    dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.layout, first_set, kBoundSets - first_set,
                                binding.sets.data() + first_set, first_set <= kConstantsSet ? kDynamicOffsets : 0,
                                binding.offsets.data());
  } else {
    ++stats_.set_binds_skipped;
  }
  if (set_dynamic_state) {
    memo_scissor_ = SetDynamicState(regs, m.viewport, m.targets.scale, m.polygonal, m.shift);
  }
  if (!trace_pass_.draws++) {
    trace_pass_.first_scissor = memo_scissor_;
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
  scaled_shift_ = 0;
  scaled_color_.reset();
  scaled_depth_.reset();
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) {
    return;
  }
  BeginSubmission();
  ScopedMicros timer(stats_.us_resolves);
  targets_->End(submissions_->main());
  ResolvePlan plan = PlanResolve(info, regs);
  if (plan.readback && plan.dest_pitch * plan.dest_texture_height * 4 >
                           uint32_t(std::max(0, REXCVAR_GET(nier_readback_limit_kb))) * 1024) {
    plan.readback = false;
  }
  submissions_->Mark(GpuCategory::kResolves, LabelId(ResolveId(plan), nullptr, nullptr));
  FlushTracePass();
  if (Tracing()) {
    trace_.push_back(fmt::format(
        "resolve [{} {:X} w{} f{}] {},{} {}x{} -> {:08X} f{} {}x{} pitch {}{}{}{}{}{}",
        plan.source.is_depth ? "d" : "c", plan.source.base_tiles, ExtentFor(plan.source).width, plan.source.format,
        plan.source_x, plan.source_y, plan.source_width, plan.source_height, plan.dest_address,
        uint32_t(plan.dest_format), plan.dest_width, plan.dest_height, plan.dest_pitch, plan.copy ? " copy" : "",
        plan.depth ? " depth" : "", plan.clear_color ? " clear-colour" : "", plan.clear_depth ? " clear-depth" : "",
        plan.readback ? " readback" : "", plan.dest_swap ? " swap" : ""));
  }
  const uint64_t readbacks = resolves_->stats().readbacks;
  resolves_->Resolve(submissions_->main(), plan, submissions_->current());
  memo_.valid = false;
  if (REXCVAR_GET(nier_readback_submit) && resolves_->stats().readbacks != readbacks &&
      resolves_->LastReadbackOfFrame(plan.dest_address)) {
    ++stats_.readback_submits;
    Flush();
  }
  bound_pipeline_ = VK_NULL_HANDLE;
  dynamic_filter_.Reset();
  binding_filter_.Reset();
  after_shadow_map_ = resolves_->TakeShadowMapResolved();
}

void NativeRenderer::PresentInto(VkCommandBuffer cmd, VkImage output, VkImageView output_view, bool written_before,
                                 uint32_t frontbuffer, uint32_t width, uint32_t height) {
  const auto& dfn = Vk(device_);
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
  const auto& dfn = Vk(device_);
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
  if (REXCVAR_GET(nier_readback_poll) && resolves_->readbacks_pending()) {
    ProcessCompleted();
  }
  if (!gpu_governor_.quality().full_scene) {
    ++stats_.reduced_scene_frames;
  }
  if (++governor_frames_ >= kGovernorFrames) {
    uint64_t gpu_ns = 0;
    for (const uint64_t ns : submissions_->gpu_ns()) {
      gpu_ns += ns;
    }
    const double window_ms = double(gpu_ns - governor_gpu_ns_) / 1e6 / double(governor_frames_);
    const auto now = std::chrono::steady_clock::now();
    const double frame_ms =
        std::chrono::duration<double, std::milli>(now - governor_started_).count() / double(governor_frames_);
    governor_started_ = now;
    const int level = gpu_governor_.level();
    gpu_governor_.Configure(CurrentQualityOptions(), REXCVAR_GET(nier_fsr_gpu_budget_ms));
    gpu_governor_.Window(window_ms, frame_ms);
    const Quality& quality = gpu_governor_.quality();
    g_scene_reduced.store(!quality.full_scene, std::memory_order_relaxed);
    if (gpu_governor_.level() != level) {
      REXLOG_INFO("[native] gpu level {} after {:.1f} ms of gpu in {:.1f} ms frames: upscale {}, sharpen {}, full "
                  "scene {}, full shadows {}",
                  gpu_governor_.level(), window_ms, frame_ms, quality.upscale, quality.sharpen, quality.full_scene,
                  quality.full_shadows);
    }
    governor_gpu_ns_ = gpu_ns;
    governor_frames_ = 0;
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
  const uint64_t now_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
  const std::array<uint64_t, 3> mark = {stats_.us_draws + stats_.us_resolves + stats_.us_present, stats_.us_gpu_wait,
                                        textures_->stats().upload_bytes};
  if (last_swap_us_) {
    slow_frames_.Add({double(now_us - last_swap_us_) / 1000.0, double(mark[0] - frame_mark_[0]) / 1000.0,
                      double(mark[1] - frame_mark_[1]) / 1000.0, double(mark[2] - frame_mark_[2]) / 1048576.0});
  }
  last_swap_us_ = now_us;
  frame_mark_ = mark;
  const uint64_t now_ms = NowMs();
  frame_clock_ms_ = AdvanceFrameClock(frame_clock_ms_, last_swap_ms_, now_ms);
  last_swap_ms_ = now_ms;
  mirror_sync_.BeginFrame();
  if (const bool cards = CardsEnabled(); cards != cards_mode_) {
    submissions_->WaitIdle();
    ProcessCompleted();
    if (cards && !mirror_cards_) {
      mirror_cards_ = std::make_unique<StoreCards>(card_table_, kMirrorCards);
    }
    cards_mode_ = cards;
    REXLOG_INFO("[native] changed guest memory found by {}", cards ? "store cards" : "hashing");
  }
  textures_->SetCards(card_table_, cards_mode_, REXCVAR_GET(nier_store_cards_verify));
  textures_->BeginFrame(frame_clock_ms_);
  resolves_->BeginFrame();
  if (frame_clock_ms_ >= last_eviction_ms_ + kEvictionPeriodMs) {
    last_eviction_ms_ = frame_clock_ms_;
    textures_->Evict(kUnusedTextureSeconds * 1000);
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
  std::array<uint32_t, 17> key;
  key.fill(~0u);
  for (uint32_t i = 0; i < set.color.size(); ++i) {
    if (const auto& color = set.color[i]) {
      key[i * 3] = color->base_tiles;
      key[i * 3 + 1] = ExtentFor(*color).width;
      key[i * 3 + 2] = uint32_t(color->format);
    }
  }
  if (set.depth) {
    key[12] = set.depth->base_tiles;
    key[13] = ExtentFor(*set.depth).width;
    key[14] = uint32_t(set.depth->format);
  }
  key[15] = set.scale.x;
  key[16] = set.scale.y;
  const uint64_t hash =
      std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(key.data()), sizeof(key)));
  if (auto it = pass_keys_.find(hash); it != pass_keys_.end()) {
    return it->second;
  }
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
  const uint16_t id = NamedPass(name, false);
  pass_keys_.emplace(hash, id);
  return id;
}

uint16_t NativeRenderer::ResolveId(const ResolvePlan& plan) {
  return NamedPass(fmt::format("{} {:X} w{} {}x{}{}{}{}", plan.source.is_depth ? "d" : "c", plan.source.base_tiles,
                               ExtentFor(plan.source).width, plan.source_width, plan.source_height,
                               plan.copy ? "" : " no copy", plan.readback ? " readback" : "",
                               plan.clear_color || plan.clear_depth ? " clear" : ""),
                   true);
}

uint16_t NativeRenderer::NamedPass(const std::string& name, bool resolve) {
  auto [it, inserted] = pass_ids_.try_emplace(name, uint16_t(pass_names_.size()));
  if (inserted) {
    pass_names_.push_back(name);
    pass_is_resolve_.push_back(resolve);
  }
  return it->second;
}

uint16_t NativeRenderer::LabelId(uint16_t pass, const vulkan::VulkanShader* pixel_shader,
                                 const vulkan::VulkanShader::VulkanTranslation* pixel) {
  const uint64_t hash = pixel_shader ? pixel_shader->ucode_data_hash() | 1 : 0;
  const uint64_t key = (uint64_t(pass) << 48) ^ (hash & 0xFFFFFFFFFFFFull);
  if (key == last_label_key_ && last_label_id_) {
    return last_label_id_;
  }
  auto [it, inserted] = label_ids_.try_emplace(key, uint16_t(label_pass_.size()));
  if (inserted) {
    label_pass_.push_back(pass);
    label_shader_.push_back(pixel_shader ? fmt::format("ps {:016X} ({} ucode dwords, {} spirv words)", hash,
                                                       pixel_shader->ucode_dword_count(),
                                                       pixel ? pixel->translated_binary().size() / 4 : 0)
                                         : std::string());
  }
  last_label_key_ = key;
  last_label_id_ = it->second;
  return it->second;
}

void NativeRenderer::AppendHeaviest(std::string& line, std::string_view title,
                                    std::vector<std::pair<uint64_t, size_t>>& heaviest) const {
  if (heaviest.empty()) {
    return;
  }
  std::sort(heaviest.rbegin(), heaviest.rend());
  heaviest.resize(std::min(heaviest.size(), kReportedPasses));
  line += fmt::format("; {}:", title);
  for (const auto& [ns, id] : heaviest) {
    line += fmt::format(" {} ms [{}],", ns / 1000000, pass_names_[id]);
  }
  line.pop_back();
}

bool NativeRenderer::Tracing() const { return traced_frames_.size() < kTracedFrames && stats_.swaps < kTracedSwaps; }

void NativeRenderer::FlushTracePass() {
  if (trace_pass_.draws && Tracing()) {
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
  if (!Tracing()) {
    return;
  }
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
      "[native] render: {}/{} draws drawn, skipped raster {} primitive {} target {} shader {} pending {} empty {}; {} resolves ({} colour, "
      "{} depth, {} clears, {} readbacks, {} stale, {} skipped, {} repeated); mirror {} copies {} MB {} waits {} staged {} MB {} flushes; textures {} uploads {} MB {} "
      "unsupported {} resolve hits {} reused, {} live {} MB; {} resolve images, {} mirror keys; {} targets, {} pipelines, {} submissions, {} black frames",
      s.drawn - r.drawn, s.draws - r.draws, s.skipped_raster - r.skipped_raster,
      s.skipped_primitive - r.skipped_primitive, s.skipped_target - r.skipped_target,
      s.skipped_shader - r.skipped_shader, s.skipped_pending - r.skipped_pending, s.skipped_empty - r.skipped_empty, s.resolves - r.resolves, v.copies - reported_resolves_.copies,
      v.depth_copies - reported_resolves_.depth_copies, v.clears - reported_resolves_.clears,
      v.readbacks - reported_resolves_.readbacks, v.stale_readbacks - reported_resolves_.stale_readbacks,
      v.skipped - reported_resolves_.skipped, v.repeated - reported_resolves_.repeated,
      s.mirror_copies - r.mirror_copies, (s.mirror_bytes - r.mirror_bytes) >> 20, s.mirror_waits - r.mirror_waits,
      s.mirror_staged - r.mirror_staged, (s.mirror_staged_bytes - r.mirror_staged_bytes) >> 20, s.mirror_flushes - r.mirror_flushes,
      t.uploads - reported_textures_.uploads, (t.upload_bytes - reported_textures_.upload_bytes) >> 20,
      t.unsupported - reported_textures_.unsupported, t.resolve_hits - reported_textures_.resolve_hits,
      t.reused - reported_textures_.reused,
      textures_->live_images(), (textures_->active_bytes() + textures_->retired_bytes() + textures_->free_bytes()) >> 20, resolves_->image_count(), mirror_sync_.key_count(), targets_->count(), pipelines_->pipeline_count(), submissions - reported_submissions_,
      s.black_frames - r.black_frames);
  const Pipelines::Stats p = pipelines_->TakeStats();
  line += fmt::format("; pipelines compiled {} in {} ms (max {} ms), cache {} KB", p.compiled - reported_pipelines_.compiled,
                      (p.compile_us - reported_pipelines_.compile_us) / 1000, p.max_compile_us / 1000,
                      p.saved_bytes >> 10);
  reported_pipelines_ = p;
  const NativeShaders::Stats& sh = shaders_.stats();
  line += fmt::format("; shaders translated {} in {} ms (max {} ms), {} from the cache, {} in the background, {} "
                      "draws waited",
                      sh.translations - reported_shaders_.translations,
                      (sh.translate_us - reported_shaders_.translate_us) / 1000, sh.max_translate_us / 1000,
                      sh.cache_hits - reported_shaders_.cache_hits, sh.background - reported_shaders_.background,
                      sh.pending_draws - reported_shaders_.pending_draws);
  reported_shaders_ = sh;
  line += fmt::format("; fsr {} runs, gpu level {}, reduced scene frames {}", upscale_->runs() - reported_upscale_runs_,
                      gpu_governor_.level(), s.reduced_scene_frames - r.reduced_scene_frames);
  reported_upscale_runs_ = upscale_->runs();
  line += fmt::format("; silent draws skipped {}", s.skipped_silent - r.skipped_silent);
  line += fmt::format("; draw memo hits {}, mismatches {}", s.memo_hits - r.memo_hits, s.memo_mismatches - r.memo_mismatches);
  {
    const Resolves::Stats& rv = resolves_->stats();
    line += fmt::format("; readbacks written after 0/1/2/3+ frames {}/{}/{}/{}, early submits {}",
                        rv.readback_frames[0] - reported_readback_frames_[0], rv.readback_frames[1] - reported_readback_frames_[1],
                        rv.readback_frames[2] - reported_readback_frames_[2], rv.readback_frames[3] - reported_readback_frames_[3],
                        s.readback_submits - r.readback_submits);
    reported_readback_frames_ = rv.readback_frames;
  }
  line += fmt::format("; background textures {} started, {} draws waited, {} dropped, {} loaded in place when full",
                      t.background_loads - reported_textures_.background_loads,
                      t.background_waits - reported_textures_.background_waits,
                      t.background_dropped - reported_textures_.background_dropped,
                      t.background_full - reported_textures_.background_full);
  line += fmt::format("; mip chains {} binds, {} level copies; texture views cached {}",
                      t.mip_chain_binds - reported_textures_.mip_chain_binds,
                      t.mip_chain_copies - reported_textures_.mip_chain_copies,
                      t.cached_views - reported_textures_.cached_views);
  line += fmt::format("; slow frames {} of {} (worst {:.0f} ms): ring busy {}, gpu wait {}, texture uploads {}, other {}",
                      slow_frames_.slow, slow_frames_.frames, slow_frames_.worst_ms, slow_frames_.ring,
                      slow_frames_.gpu, slow_frames_.uploads, slow_frames_.other);
  slow_frames_ = {};
  if (recorder_) {
    const std::array<uint64_t, 5> now = {recorder_->commands(), recorder_->drain_waits(), recorder_->drain_wait_us(),
                                         recorder_->full_waits(), recorder_->wakeups()};
    line += fmt::format("; record thread {}: {} commands, {} wakeups, {} drain waits ({} ms), {} full waits",
                        ActiveRecorder() ? "on" : "off", now[0] - reported_recorder_[0],
                        now[4] - reported_recorder_[4], now[1] - reported_recorder_[1],
                        (now[2] - reported_recorder_[2]) / 1000, now[3] - reported_recorder_[3]);
    reported_recorder_ = now;
  }
  if (mirror_cards_) {
    const std::array<uint64_t, 3> pages = {mirror_cards_->checked_pages(), mirror_cards_->taken_pages(),
                                           mirror_cards_->rechecked_pages()};
    line += fmt::format("; store cards: mirror checked {} pages, took {} ({} rechecks), copied {} MB, missed {}; "
                        "textures reloaded {}, marked but unchanged {}, missed {}",
                        pages[0] - reported_card_pages_[0], pages[1] - reported_card_pages_[1],
                        pages[2] - reported_card_pages_[2], (s.cards_copied_bytes - r.cards_copied_bytes) >> 20,
                        s.cards_missed - r.cards_missed, t.cards_reloads - reported_textures_.cards_reloads,
                        t.cards_unchanged - reported_textures_.cards_unchanged,
                        t.cards_missed - reported_textures_.cards_missed);
    reported_card_pages_ = pages;
  }
  line += fmt::format("; draw state: dynamic {} set {} skipped, sets bound {} skipped {}, uniforms written {} "
                      "reused {} (unpacked {}, stale {})",
                      s.dynamic_calls - r.dynamic_calls, s.dynamic_skipped - r.dynamic_skipped,
                      s.set_binds - r.set_binds, s.set_binds_skipped - r.set_binds_skipped,
                      s.uniforms_written - r.uniforms_written, s.uniforms_reused - r.uniforms_reused,
                      s.uniforms_unpacked - r.uniforms_unpacked, s.uniforms_stale - r.uniforms_stale);
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
  line += fmt::format("; gpu ms draws {}, resolves {}, transfers {}, uploads {}, present {}",
                      gpu_ms(GpuCategory::kDraws), gpu_ms(GpuCategory::kResolves), gpu_ms(GpuCategory::kTransfers),
                      gpu_ms(GpuCategory::kUploads), gpu_ms(GpuCategory::kPresent));
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
  std::vector<std::pair<uint64_t, size_t>> heaviest_resolves;
  for (size_t i = 1; i < per_pass.size(); ++i) {
    if (per_pass[i]) {
      (pass_is_resolve_[i] ? heaviest_resolves : heaviest).emplace_back(per_pass[i], i);
    }
  }
  AppendHeaviest(line, "gpu passes", heaviest);
  AppendHeaviest(line, "gpu resolves", heaviest_resolves);
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
  line += fmt::format("; edram transfers {} ({} tiles in {} regions, {} from depth), {} incompatible tiles",
                      e.transfers - reported_targets_.transfers, e.transfer_tiles - reported_targets_.transfer_tiles,
                      e.transfer_regions - reported_targets_.transfer_regions,
                      e.depth_tiles - reported_targets_.depth_tiles,
                      e.incompatible_tiles - reported_targets_.incompatible_tiles);
  reported_targets_ = e;
  line += fmt::format("; {} evictions, {} memory skips, {} releases, {} draw retries, {} skipped after retry, {} staging flushes",
                      t.evictions - rt.evictions, t.memory_failures - rt.memory_failures, t.releases - rt.releases,
                      s.draw_retries - r.draw_retries, s.skipped_retry - r.skipped_retry,
                      s.staging_flushes - r.staging_flushes);
  line += fmt::format("; memory MB: mirror {}, textures {} (retired {}, free {}, arena {}: {} images, {} own), resolve images {}, targets {}, staging {}, uniforms {}",
                      GuestMirror::kSize >> 20, textures_->active_bytes() >> 20, textures_->retired_bytes() >> 20,
                      textures_->free_bytes() >> 20, textures_->arena_bytes() >> 20, t.arena_images - rt.arena_images,
                      t.dedicated_images - rt.dedicated_images,
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
