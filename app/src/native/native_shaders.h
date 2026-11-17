#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

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

  explicit NativeShaders(const rex::ui::vulkan::VulkanDevice* device);

  void Load(uint32_t type, const uint32_t* words_big_endian, uint32_t size_words);
  void PrepareDraw(const rex::graphics::RegisterFile& regs);
  const Stats& stats() const { return stats_; }

 private:
  using Modification = rex::graphics::SpirvShaderTranslator::Modification;

  Modification VertexModification(const rex::graphics::Shader& shader,
                                  rex::graphics::Shader::HostVertexShaderType host_type,
                                  uint32_t interpolator_mask, bool param_gen_used,
                                  const rex::graphics::RegisterFile& regs) const;
  Modification PixelModification(const rex::graphics::Shader& shader, uint32_t interpolator_mask,
                                 uint32_t param_gen_pos, const rex::graphics::RegisterFile& regs) const;
  void Analyze(rex::graphics::vulkan::VulkanShader& shader);
  void Translate(rex::graphics::vulkan::VulkanShader& shader, Modification modification);

  const rex::ui::vulkan::VulkanDevice* device_;
  rex::graphics::SpirvShaderTranslator translator_;
  rex::string::StringBuffer disassembly_;
  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::vulkan::VulkanShader>> shaders_;
  rex::graphics::vulkan::VulkanShader* vertex_shader_ = nullptr;
  rex::graphics::vulkan::VulkanShader* pixel_shader_ = nullptr;
  Stats stats_;
};

}  // namespace nier::native
