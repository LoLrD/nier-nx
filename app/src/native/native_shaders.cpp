#include "native_shaders.h"

#include <algorithm>
#include <chrono>

#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>
#include <rex/graphics/util/draw.h>

#include "perf_modes.h"

namespace nier::native {

using namespace rex::graphics;
using rex::graphics::vulkan::VulkanShader;

namespace {

using SystemConstants = SpirvShaderTranslator::SystemConstants;

constexpr ByteRange FieldRange(size_t offset, size_t size) {
  return {uint32_t(offset), uint32_t(offset + size)};
}

constexpr std::array<ByteRange, 3> kSpecializedSystemFields = {{
    FieldRange(offsetof(SystemConstants, flags), sizeof(SystemConstants::flags)),
    FieldRange(offsetof(SystemConstants, texture_swizzled_signs), sizeof(SystemConstants::texture_swizzled_signs)),
    FieldRange(offsetof(SystemConstants, texture_swizzles), sizeof(SystemConstants::texture_swizzles)),
}};

SpirvShaderTranslator::Features TranslatorFeatures(const rex::ui::vulkan::VulkanDevice* device) {
  SpirvShaderTranslator::Features features(device);
  features.image_view_format_swizzle = false;
  return features;
}

}  // namespace

NativeShaders::NativeShaders(const rex::ui::vulkan::VulkanDevice* device) : device_(device) {
  const std::array<std::pair<uint32_t, uint32_t>, kScales> scales = {{{1, 1}, {1, 2}, {2, 2}}};
  for (uint32_t i = 0; i < kScales; ++i) {
    translators_[i] = std::make_unique<SpirvShaderTranslator>(TranslatorFeatures(device), false, false, false,
                                                              scales[i].first, scales[i].second);
  }
}

NativeShaders::~NativeShaders() {
  for (auto& [translation, specialization] : specializations_) {
    for (auto& [key, module] : specialization.modules) {
      device_->functions().vkDestroyShaderModule(device_->device(), module, nullptr);
    }
  }
}

VkShaderModule NativeShaders::PixelModule(VulkanShader::VulkanTranslation& translation,
                                          const SystemConstants& system, std::span<const uint32_t> fetch_constants,
                                          bool early_tests) {
  return SpecializedModule(translation, nullptr, system, fetch_constants, early_tests);
}

VkShaderModule NativeShaders::VertexModule(const VulkanShader& shader, VulkanShader::VulkanTranslation& translation,
                                           const SystemConstants& system, std::span<const uint32_t> fetch_constants) {
  return SpecializedModule(translation, &shader, system, fetch_constants, false);
}

VkShaderModule NativeShaders::SpecializedModule(VulkanShader::VulkanTranslation& translation,
                                                const VulkanShader* vertex_shader, const SystemConstants& system,
                                                std::span<const uint32_t> fetch_constants, bool early_tests) {
  const std::vector<uint8_t>& binary = translation.translated_binary();
  const std::span<const uint32_t> words(reinterpret_cast<const uint32_t*>(binary.data()), binary.size() / 4);
  auto [it, inserted] = specializations_.try_emplace(&translation);
  Specialization& specialization = it->second;
  if (inserted) {
    specialization.slots = SpecializableSlots(words, kSpecializedSystemFields);
    if (vertex_shader) {
      std::vector<uint32_t> textures;
      for (const auto& binding : vertex_shader->GetTextureBindingsAfterTranslation()) {
        textures.push_back(binding.fetch_constant);
      }
      specialization.slots = TextureFetchSlots(std::move(specialization.slots), textures);
    }
  }
  const bool contract = !vertex_shader && !PerfSkip(kExactMath);
  if (specialization.slots.empty() && !contract && !early_tests) {
    return translation.shader_module();
  }
  const auto* system_words = reinterpret_cast<const uint32_t*>(&system);
  std::vector<uint32_t>& values = specialization_values_;
  values.clear();
  for (const UniformSlot& slot : specialization.slots) {
    values.push_back(slot.fetch ? fetch_constants[slot.offset / 4] : system_words[slot.offset / 4]);
  }
  LastModule& last = last_modules_[vertex_shader ? 1 : 0];
  if (last.translation == &translation && last.contract == contract && last.early_tests == early_tests &&
      last.values == values) {
    return last.module;
  }
  auto [module, created] = specialization.modules.try_emplace(
      XXH3_64bits_withSeed(values.data(), values.size() * sizeof(uint32_t), uint64_t(contract) | uint64_t(early_tests) << 1),
      VK_NULL_HANDLE);
  if (created) {
    std::vector<uint32_t> specialized = Specialize(words, kSpecializedSystemFields, specialization.slots, values);
    if (contract) {
      specialized = AllowContraction(specialized);
    }
    if (early_tests) {
      specialized = ForceEarlyFragmentTests(specialized);
    }
    module->second =
        rex::ui::vulkan::util::CreateShaderModule(device_, specialized.data(), specialized.size() * sizeof(uint32_t));
    ++stats_.specialized;
  }
  const VkShaderModule result = module->second != VK_NULL_HANDLE ? module->second : translation.shader_module();
  last.translation = &translation;
  last.values = values;
  last.contract = contract;
  last.early_tests = early_tests;
  last.module = result;
  return result;
}

uint32_t NativeShaders::ScaleIndex(uint32_t scale_x, uint32_t scale_y) {
  return scale_x > 1 ? 2 : (scale_y > 1 ? 1 : 0);
}

void NativeShaders::Load(uint32_t type, const uint32_t* words_big_endian, uint32_t size_words) {
  const uint64_t hash = XXH3_64bits(words_big_endian, size_t(size_words) * sizeof(uint32_t));
  uint64_t& current = type == uint32_t(xenos::ShaderType::kVertex) ? vertex_hash_ : pixel_hash_;
  if (current == hash) {
    return;
  }
  auto& entry = microcode_[hash];
  if (entry.second.empty()) {
    entry = {type, std::vector<uint32_t>(words_big_endian, words_big_endian + size_words)};
  }
  current = hash;
}

VulkanShader* NativeShaders::ShaderFor(uint64_t hash, uint32_t scale) {
  auto& shader = shaders_[scale][hash];
  if (!shader) {
    const auto& [type, words] = microcode_.at(hash);
    shader = std::make_unique<VulkanShader>(device_, xenos::ShaderType(type), hash, words.data(),
                                            uint32_t(words.size()));
  }
  return shader.get();
}

std::optional<NativeShaders::PreparedDraw> NativeShaders::PrepareDraw(const RegisterFile& regs,
                                                                       Shader::HostVertexShaderType host_type,
                                                                       uint32_t scale_x, uint32_t scale_y,
                                                                       uint32_t position_shift) {
  if (!vertex_hash_) {
    return std::nullopt;
  }
  VulkanShader* vertex_shader = ShaderFor(vertex_hash_, 0);
  Analyze(*vertex_shader);
  const uint32_t scale = ScaleIndex(scale_x, scale_y);
  VulkanShader* pixel_shader =
      pixel_hash_ && regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth
          ? ShaderFor(pixel_hash_, scale)
          : nullptr;
  if (pixel_shader) {
    Analyze(*pixel_shader);
  }
  uint32_t param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      pixel_shader ? vertex_shader->writes_interpolators() &
                         pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                                regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_pos)
                   : 0;
  PreparedDraw prepared{vertex_shader, nullptr, pixel_shader, nullptr,
                        pixel_shader && param_gen_pos < xenos::kMaxInterpolators};
  prepared.vertex =
      Translate(*translators_[0], *vertex_shader,
                VertexModification(*vertex_shader, host_type, interpolator_mask, param_gen_pos != UINT32_MAX, regs));
  if (!prepared.vertex) {
    return std::nullopt;
  }
  if (pixel_shader) {
    prepared.pixel = Translate(*translators_[scale], *pixel_shader,
                               PixelModification(*pixel_shader, interpolator_mask, param_gen_pos, position_shift, regs));
    if (!prepared.pixel) {
      return std::nullopt;
    }
  }
  return prepared;
}

NativeShaders::Modification NativeShaders::VertexModification(const Shader& shader,
                                                              Shader::HostVertexShaderType host_type,
                                                              uint32_t interpolator_mask, bool param_gen_used,
                                                              const RegisterFile& regs) const {
  const auto& properties = device_->properties();
  Modification modification(translators_[0]->GetDefaultVertexShaderModification(
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
                                                             uint32_t param_gen_pos, uint32_t position_shift,
                                                             const RegisterFile& regs) const {
  using DepthStencilMode = Modification::DepthStencilMode;
  Modification modification(translators_[0]->GetDefaultPixelShaderModification(
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
    modification.pixel.position_shift = position_shift;
  }
  modification.pixel.depth_stencil_mode =
      shader.implicit_early_z_write_allowed() &&
              (!shader.writes_color_target(0) ||
               !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
          ? DepthStencilMode::kEarlyHint
          : DepthStencilMode::kNoModifiers;
  modification.pixel.implicit_lod = !PerfSkip(kExplicitGradients) && !PerfSkip(kGenericShaders);
  return modification;
}

void NativeShaders::Analyze(VulkanShader& shader) {
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(disassembly_);
  }
}

VulkanShader::VulkanTranslation* NativeShaders::Translate(SpirvShaderTranslator& translator, VulkanShader& shader,
                                                          Modification modification) {
  bool is_new = false;
  auto* translation =
      static_cast<VulkanShader::VulkanTranslation*>(shader.GetOrCreateTranslation(modification.value, &is_new));
  if (!is_new) {
    return translation->shader_module() != VK_NULL_HANDLE ? translation : nullptr;
  }
  const auto start = std::chrono::steady_clock::now();
  const bool ok = translator.TranslateAnalyzedShader(*translation) &&
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
    return nullptr;
  }
  return translation;
}

}  // namespace nier::native
