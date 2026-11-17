#include "native_shaders.h"

#include <algorithm>
#include <chrono>

#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/graphics/util/draw.h>

#include "host_vertex_type.h"

namespace nier::native {

using namespace rex::graphics;
using rex::graphics::vulkan::VulkanShader;

namespace {

SpirvShaderTranslator::Features TranslatorFeatures(const rex::ui::vulkan::VulkanDevice* device) {
  SpirvShaderTranslator::Features features(device);
  features.image_view_format_swizzle = false;
  return features;
}

}  // namespace

NativeShaders::NativeShaders(const rex::ui::vulkan::VulkanDevice* device)
    : device_(device), translator_(TranslatorFeatures(device), false, false, false) {}

void NativeShaders::Load(uint32_t type, const uint32_t* words_big_endian, uint32_t size_words) {
  const uint64_t hash = XXH3_64bits(words_big_endian, size_t(size_words) * sizeof(uint32_t));
  auto& shader = shaders_[hash];
  if (!shader) {
    shader = std::make_unique<VulkanShader>(device_, xenos::ShaderType(type), hash, words_big_endian, size_words);
  }
  (type == uint32_t(xenos::ShaderType::kVertex) ? vertex_shader_ : pixel_shader_) = shader.get();
}

void NativeShaders::PrepareDraw(const RegisterFile& regs) {
  if (!vertex_shader_) {
    return;
  }
  Analyze(*vertex_shader_);
  VulkanShader* pixel_shader =
      regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth ? pixel_shader_ : nullptr;
  if (pixel_shader) {
    Analyze(*pixel_shader);
  }
  uint32_t param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      pixel_shader ? vertex_shader_->writes_interpolators() &
                         pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                                regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_pos)
                   : 0;
  const auto host_type = HostVertexShaderTypeFor(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type);
  Translate(*vertex_shader_,
            VertexModification(*vertex_shader_, host_type, interpolator_mask, param_gen_pos != UINT32_MAX, regs));
  if (pixel_shader) {
    Translate(*pixel_shader, PixelModification(*pixel_shader, interpolator_mask, param_gen_pos, regs));
  }
}

NativeShaders::Modification NativeShaders::VertexModification(const Shader& shader,
                                                              Shader::HostVertexShaderType host_type,
                                                              uint32_t interpolator_mask, bool param_gen_used,
                                                              const RegisterFile& regs) const {
  const auto& properties = device_->properties();
  Modification modification(translator_.GetDefaultVertexShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg), host_type));
  modification.vertex.interpolator_mask = interpolator_mask;
  modification.vertex.tessellation_mode =
      Shader::IsHostVertexShaderTypeDomain(host_type) ? uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode) : 0;
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = clip.clip_disable ? 0 : clip.ucp_ena;
  modification.vertex.user_clip_plane_cull = 0;
  if (user_clip_planes) {
    if ((clip.ucp_cull_only_ena && properties.shaderCullDistance) ||
        (!properties.shaderClipDistance && properties.shaderCullDistance)) {
      modification.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance) {
      user_clip_planes = 0;
    }
  }
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = clip.ps_ucp_mode;
  modification.vertex.output_point_parameters =
      host_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip
          ? uint32_t(param_gen_used)
          : uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                     regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  modification.vertex.vertex_kill_and = uint32_t(properties.shaderCullDistance &&
                                                 (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
                                                 !clip.vtx_kill_or);
  return modification;
}

NativeShaders::Modification NativeShaders::PixelModification(const Shader& shader, uint32_t interpolator_mask,
                                                             uint32_t param_gen_pos,
                                                             const RegisterFile& regs) const {
  using DepthStencilMode = Modification::DepthStencilMode;
  Modification modification(translator_.GetDefaultPixelShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                                                 regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                                                 regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  modification.pixel.depth_stencil_mode =
      shader.implicit_early_z_write_allowed() &&
              (!shader.writes_color_target(0) ||
               !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
          ? DepthStencilMode::kEarlyHint
          : DepthStencilMode::kNoModifiers;
  return modification;
}

void NativeShaders::Analyze(VulkanShader& shader) {
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(disassembly_);
  }
}

void NativeShaders::Translate(VulkanShader& shader, Modification modification) {
  bool is_new = false;
  auto* translation =
      static_cast<VulkanShader::VulkanTranslation*>(shader.GetOrCreateTranslation(modification.value, &is_new));
  if (!is_new) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const bool ok = translator_.TranslateAnalyzedShader(*translation) &&
                  translation->GetOrCreateShaderModule() != VK_NULL_HANDLE;
  const auto us = uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  ++stats_.translations;
  stats_.translate_us += us;
  stats_.max_translate_us = std::max(stats_.max_translate_us, us);
  if (!ok) {
    ++stats_.failures;
    REXLOG_ERROR("[native] shader {:016X} modification {:016X} failed to translate", shader.ucode_data_hash(),
                 modification.value);
  }
}

}  // namespace nier::native
