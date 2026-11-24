#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/ui/vulkan/api.h>

#include "draw_constants.h"
#include "draw_state_filter.h"
#include "gpu_registers.h"
#include "guest_mirror.h"
#include "mirror_sync.h"
#include "native_shaders.h"
#include "pipeline_report.h"
#include "pipelines.h"
#include "render_targets.h"
#include "resolve_plan.h"
#include "resolves.h"
#include "slow_frames.h"
#include "store_cards.h"
#include "submissions.h"
#include "target_keys.h"
#include "textures.h"
#include "vk_recorder.h"

namespace rex::memory {
class Memory;
}

namespace rex::ui {
class Presenter;
namespace vulkan {
class VulkanDevice;
class VulkanUploadBufferPool;
}  // namespace vulkan
}  // namespace rex::ui

namespace nier::native {

class NativeRenderer {
 public:
  static constexpr uint32_t kMirrorRehashBytes = 64 << 10;

  struct Stats {
    uint64_t draws = 0;
    uint64_t drawn = 0;
    uint64_t skipped_raster = 0;
    uint64_t skipped_primitive = 0;
    uint64_t skipped_target = 0;
    uint64_t skipped_shader = 0;
    uint64_t skipped_pending = 0;
    uint64_t skipped_empty = 0;
    uint64_t resolves = 0;
    uint64_t swaps = 0;
    uint64_t black_frames = 0;
    uint64_t presented_changes = 0;
    uint64_t draw_retries = 0;
    uint64_t skipped_retry = 0;
    uint64_t memory_releases = 0;
    uint64_t staging_flushes = 0;
    uint64_t mirror_copies = 0;
    uint64_t mirror_bytes = 0;
    uint64_t mirror_waits = 0;
    uint64_t mirror_staged = 0;
    uint64_t mirror_staged_bytes = 0;
    uint64_t mirror_flushes = 0;
    uint64_t dynamic_calls = 0;
    uint64_t dynamic_skipped = 0;
    uint64_t set_binds = 0;
    uint64_t set_binds_skipped = 0;
    uint64_t uniforms_written = 0;
    uint64_t uniforms_reused = 0;
    uint64_t cards_copied_bytes = 0;
    uint64_t cards_missed = 0;
    uint64_t us_draws = 0;
    uint64_t us_mirror = 0;
    uint64_t us_textures = 0;
    uint64_t us_gpu_wait = 0;
    uint64_t us_resolves = 0;
    uint64_t us_present = 0;
    uint64_t us_presenter = 0;
  };

  NativeRenderer(const rex::ui::vulkan::VulkanDevice* device, rex::memory::Memory* memory, NativeShaders& shaders,
                 const GpuRegisters& registers);
  ~NativeRenderer();
  NativeRenderer(const NativeRenderer&) = delete;
  NativeRenderer& operator=(const NativeRenderer&) = delete;

  bool valid() const { return valid_; }
  void SetFrameDump(std::string directory, uint32_t interval);
  void Draw(const rex::graphics::RegisterFile& regs);
  void Resolve(const rex::graphics::RegisterFile& regs);
  void Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height);
  std::string Report();
  VulkanRecorder* recorder() const { return recorder_.get(); }

 private:
  enum class DrawResult { kDone, kRetry, kOutOfMemory };

  struct Presented {
    bool found = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const Presented&) const = default;
  };

  DrawResult RecordDraw(const rex::graphics::RegisterFile& regs);
  uint16_t PassId(const TargetSet& set);
  uint16_t ResolveId(const ResolvePlan& plan);
  uint16_t NamedPass(const std::string& name, bool resolve);
  void AppendHeaviest(std::string& line, std::string_view title,
                      std::vector<std::pair<uint64_t, size_t>>& heaviest) const;
  bool Tracing() const;
  void FlushTracePass();
  void TraceSwap(uint32_t frontbuffer, uint32_t width, uint32_t height);
  uint16_t LabelId(uint16_t pass, const rex::graphics::vulkan::VulkanShader* pixel_shader,
                   const rex::graphics::vulkan::VulkanShader::VulkanTranslation* pixel);
  void ReleaseMemory();
  void BeginSubmission();
  void Flush();
  void WaitFor(uint64_t submission);
  void ProcessCompleted();
  void SyncMirror(uint32_t address, uint32_t size);
  void CopyMirror(uint32_t address, uint32_t size);
  void SyncMirrorCards(uint32_t address, uint32_t size);
  void CopyMirrorRun(const PageRun& run);
  void VerifyMirror(uint32_t address, uint32_t size);
  bool CardsEnabled() const;
  VkDescriptorSet EmptyTextureSet();
  VkDescriptorSet BindTextures(const rex::graphics::RegisterFile& regs, const rex::graphics::SpirvShader& shader,
                               bool pixel, TextureConstantInfo& info, uint32_t& textures, uint32_t& samplers,
                               bool& out_of_memory);
  uint32_t DownscaleFor(const TargetSet& targets, bool writes_depth);
  VkRect2D SetDynamicState(const rex::graphics::RegisterFile& regs, const rex::graphics::draw_util::ViewportInfo& viewport,
                       const SampleScale& scale, bool polygonal, uint32_t shift);
  void DumpFrame(uint32_t frontbuffer, uint32_t width, uint32_t height);
  void PresentInto(VkCommandBuffer command_buffer, VkImage output, VkImageView output_view, bool written_before,
                   uint32_t frontbuffer, uint32_t width, uint32_t height);
  void CreateGammaPipeline();

  const rex::ui::vulkan::VulkanDevice* device_;
  rex::memory::Memory* memory_;
  uint8_t* physical_base_;
  NativeShaders& shaders_;
  const GpuRegisters& registers_;
  rex::graphics::DrawExtentEstimator extents_;
  bool valid_ = false;
  std::unique_ptr<PipelineReport> pipeline_report_;
  std::unique_ptr<Pipelines> pipelines_;
  std::unique_ptr<Submissions> submissions_;
  std::unique_ptr<GuestMirror> mirror_;
  std::unique_ptr<RenderTargets> targets_;
  std::unique_ptr<rex::ui::vulkan::VulkanUploadBufferPool> staging_;
  std::unique_ptr<Textures> textures_;
  std::unique_ptr<Resolves> resolves_;
  MirrorSync mirror_sync_{kMirrorRehashBytes};
  uint8_t* card_table_;
  std::unique_ptr<VulkanRecorder> recorder_;
  std::array<uint64_t, 4> reported_recorder_{};
  std::unique_ptr<StoreCards> mirror_cards_;
  PageReaders mirror_readers_;
  std::vector<PageRun> card_runs_;
  bool cards_mode_ = false;
  uint32_t reported_cards_misses_ = 0;
  std::array<uint64_t, 3> reported_card_pages_{};
  VkBuffer builtin_indices_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_memory_ = VK_NULL_HANDLE;
  VkDeviceSize quad_indices_offset_ = 0;
  VkDescriptorPool shared_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_set_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout gamma_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout gamma_layout_ = VK_NULL_HANDLE;
  VkPipeline gamma_pipeline_ = VK_NULL_HANDLE;
  VkSampler gamma_sampler_ = VK_NULL_HANDLE;
  std::string dump_directory_;
  uint32_t dump_interval_ = 0;
  uint64_t processed_ = 0;
  uint64_t last_eviction_ms_ = 0;
  uint64_t frame_clock_ms_ = 0;
  uint64_t last_swap_ms_ = 0;
  SlowFrames slow_frames_;
  uint64_t last_swap_us_ = 0;
  std::array<uint64_t, 3> frame_mark_{};
  std::deque<uint64_t> swap_marks_;
  Presented presented_;
  Stats stats_;
  Stats reported_;
  struct MirrorCounters {
    uint64_t hashed = 0;
    uint64_t sampled = 0;
    uint64_t sampled_changes = 0;
    uint64_t possibly_late = 0;
  };
  MirrorCounters reported_mirror_;
  Textures::Stats reported_textures_;
  Resolves::Stats reported_resolves_;
  Pipelines::Stats reported_pipelines_;
  uint64_t reported_submissions_ = 0;
  std::array<uint64_t, kGpuCategories> reported_gpu_ns_{};
  std::vector<uint64_t> reported_pass_ns_;
  std::unordered_map<std::string, uint16_t> pass_ids_;
  std::unordered_map<uint64_t, uint16_t> pass_keys_;
  std::vector<std::string> pass_names_{std::string()};
  std::vector<bool> pass_is_resolve_{false};
  std::unordered_map<uint64_t, uint16_t> label_ids_;
  std::vector<uint16_t> label_pass_{0};
  std::vector<std::string> label_shader_{std::string()};
  uint16_t current_pass_ = 0;
  RenderTargets::Stats reported_targets_;
  struct TracePass {
    uint16_t pass = 0;
    uint32_t draws = 0;
    VkRect2D first_scissor{};
    std::set<std::pair<uint32_t, uint32_t>> samples;
  };
  TracePass trace_pass_;
  std::vector<std::string> trace_;
  std::vector<VkDescriptorImageInfo> bind_images_;
  std::vector<VkWriteDescriptorSet> bind_writes_;
  std::vector<uint64_t> bind_handles_;
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  uint64_t bound_submission_ = 0;
  DynamicStateFilter dynamic_filter_;
  DescriptorBindingFilter binding_filter_;
  uint64_t filter_submission_ = 0;
  std::optional<TargetKey> scaled_color_;
  std::optional<TargetKey> scaled_depth_;
  uint32_t scaled_shift_ = 0;
  bool after_shadow_map_ = false;
  std::set<std::vector<uint16_t>> traced_frames_;
  std::vector<uint16_t> frame_passes_;
};

}  // namespace nier::native
