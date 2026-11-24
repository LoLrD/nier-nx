#pragma once

#include <array>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/string/buffer.h>
#include <rex/ui/vulkan/api.h>

#include "shader_cache.h"
#include "spirv_specialize.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class NativeShaders {
 public:
  struct Stats {
    uint64_t translations = 0;
    uint64_t failures = 0;
    uint64_t translate_us = 0;
    uint64_t max_translate_us = 0;
    uint64_t specialized = 0;
    uint64_t cache_hits = 0;
  };

  struct PreparedDraw {
    rex::graphics::vulkan::VulkanShader* vertex_shader;
    rex::graphics::vulkan::VulkanShader::VulkanTranslation* vertex;
    rex::graphics::vulkan::VulkanShader* pixel_shader;
    rex::graphics::vulkan::VulkanShader::VulkanTranslation* pixel;
    bool pixel_position;
  };

  explicit NativeShaders(const rex::ui::vulkan::VulkanDevice* device);
  ~NativeShaders();
  NativeShaders(const NativeShaders&) = delete;
  NativeShaders& operator=(const NativeShaders&) = delete;

  void Load(uint32_t type, const uint32_t* words_big_endian, uint32_t size_words);
  std::optional<PreparedDraw> PrepareDraw(const rex::graphics::RegisterFile& regs,
                                          rex::graphics::Shader::HostVertexShaderType host_type, uint32_t scale_x,
                                          uint32_t scale_y, uint32_t position_shift);
  VkShaderModule PixelModule(rex::graphics::vulkan::VulkanShader::VulkanTranslation& translation,
                             const rex::graphics::SpirvShaderTranslator::SystemConstants& system,
                             std::span<const uint32_t> fetch_constants, bool early_tests);
  VkShaderModule VertexModule(const rex::graphics::vulkan::VulkanShader& shader,
                              rex::graphics::vulkan::VulkanShader::VulkanTranslation& translation,
                              const rex::graphics::SpirvShaderTranslator::SystemConstants& system,
                              std::span<const uint32_t> fetch_constants);
  const Stats& stats() const { return stats_; }
  bool Current(uint64_t vertex_hash, uint64_t pixel_hash) const {
    return vertex_hash_ == vertex_hash && pixel_hash_ == pixel_hash;
  }
  void CurrentHashes(uint64_t& vertex_hash, uint64_t& pixel_hash) const {
    vertex_hash = vertex_hash_;
    pixel_hash = pixel_hash_;
  }

 private:
  struct Specialization {
    std::vector<UniformSlot> slots;
    std::unordered_map<uint64_t, VkShaderModule> modules;
  };

  using Modification = rex::graphics::SpirvShaderTranslator::Modification;

  Modification VertexModification(const rex::graphics::Shader& shader,
                                  rex::graphics::Shader::HostVertexShaderType host_type,
                                  uint32_t interpolator_mask, bool param_gen_used,
                                  const rex::graphics::RegisterFile& regs) const;
  Modification PixelModification(const rex::graphics::Shader& shader, uint32_t interpolator_mask,
                                 uint32_t param_gen_pos, uint32_t position_shift,
                                 const rex::graphics::RegisterFile& regs) const;
  static constexpr uint32_t kScales = 3;

  static uint32_t ScaleIndex(uint32_t scale_x, uint32_t scale_y);
  VkShaderModule SpecializedModule(rex::graphics::vulkan::VulkanShader::VulkanTranslation& translation,
                                   const rex::graphics::vulkan::VulkanShader* vertex_shader,
                                   const rex::graphics::SpirvShaderTranslator::SystemConstants& system,
                                   std::span<const uint32_t> fetch_constants, bool early_tests);
  rex::graphics::vulkan::VulkanShader* ShaderFor(uint64_t hash, uint32_t scale);
  void Analyze(rex::graphics::vulkan::VulkanShader& shader);
  void OpenCache(const std::string& path);
  rex::graphics::vulkan::VulkanShader::VulkanTranslation* Translate(rex::graphics::SpirvShaderTranslator& translator,
                                                                    uint32_t variant,
                                                                    rex::graphics::vulkan::VulkanShader& shader,
                                                                    Modification modification);

  const rex::ui::vulkan::VulkanDevice* device_;
  std::array<std::unique_ptr<rex::graphics::SpirvShaderTranslator>, kScales> translators_;
  rex::string::StringBuffer disassembly_;
  std::unordered_map<uint64_t, std::pair<uint32_t, std::vector<uint32_t>>> microcode_;
  std::array<std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::vulkan::VulkanShader>>, kScales> shaders_;
  uint64_t vertex_hash_ = 0;
  uint64_t pixel_hash_ = 0;
  std::unordered_map<const rex::graphics::vulkan::VulkanShader::VulkanTranslation*, Specialization> specializations_;
  struct LastModule {
    const rex::graphics::vulkan::VulkanShader::VulkanTranslation* translation = nullptr;
    std::vector<uint32_t> values;
    bool contract = false;
    bool early_tests = false;
    VkShaderModule module = VK_NULL_HANDLE;
  };

  std::vector<uint32_t> specialization_values_;
  std::array<LastModule, 2> last_modules_;
  Stats stats_;
  ShaderCache cache_;
  std::FILE* cache_file_ = nullptr;
};

}  // namespace nier::native
