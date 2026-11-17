#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/string/buffer.h>

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
  };

  struct PreparedDraw {
    rex::graphics::vulkan::VulkanShader* vertex_shader;
    rex::graphics::vulkan::VulkanShader::VulkanTranslation* vertex;
    rex::graphics::vulkan::VulkanShader* pixel_shader;
    rex::graphics::vulkan::VulkanShader::VulkanTranslation* pixel;
  };

  explicit NativeShaders(const rex::ui::vulkan::VulkanDevice* device);

  void Load(uint32_t type, const uint32_t* words_big_endian, uint32_t size_words);
  std::optional<PreparedDraw> PrepareDraw(const rex::graphics::RegisterFile& regs,
                                          rex::graphics::Shader::HostVertexShaderType host_type, uint32_t scale_x,
                                          uint32_t scale_y);
  const Stats& stats() const { return stats_; }

 private:
  using Modification = rex::graphics::SpirvShaderTranslator::Modification;

  Modification VertexModification(const rex::graphics::Shader& shader,
                                  rex::graphics::Shader::HostVertexShaderType host_type,
                                  uint32_t interpolator_mask, bool param_gen_used,
                                  const rex::graphics::RegisterFile& regs) const;
  Modification PixelModification(const rex::graphics::Shader& shader, uint32_t interpolator_mask,
                                 uint32_t param_gen_pos, const rex::graphics::RegisterFile& regs) const;
  static constexpr uint32_t kScales = 3;

  static uint32_t ScaleIndex(uint32_t scale_x, uint32_t scale_y);
  rex::graphics::vulkan::VulkanShader* ShaderFor(uint64_t hash, uint32_t scale);
  void Analyze(rex::graphics::vulkan::VulkanShader& shader);
  rex::graphics::vulkan::VulkanShader::VulkanTranslation* Translate(rex::graphics::SpirvShaderTranslator& translator,
                                                                    rex::graphics::vulkan::VulkanShader& shader,
                                                                    Modification modification);

  const rex::ui::vulkan::VulkanDevice* device_;
  std::array<std::unique_ptr<rex::graphics::SpirvShaderTranslator>, kScales> translators_;
  rex::string::StringBuffer disassembly_;
  std::unordered_map<uint64_t, std::pair<uint32_t, std::vector<uint32_t>>> microcode_;
  std::array<std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::vulkan::VulkanShader>>, kScales> shaders_;
  uint64_t vertex_hash_ = 0;
  uint64_t pixel_hash_ = 0;
  Stats stats_;
};

}  // namespace nier::native
