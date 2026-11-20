#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <SPIRV/spirv.hpp>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
using rex::graphics::xenos::ShaderType;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

namespace {

constexpr uint32_t kTextureFetch = 1;
constexpr uint32_t kExecEnd = 2;
constexpr uint32_t kUseFetchConstFilter = 3;
constexpr uint32_t kUseFetchConstAniso = 7;
constexpr uint32_t kDimension2D = 1;

std::vector<uint32_t> TextureFetchShader(bool register_gradients) {
  const uint32_t exec_word_0 = 1 | 1u << 12 | 1u << 16;
  const uint32_t exec_word_1 = kExecEnd << 12;
  const uint32_t fetch_word_0 = kTextureFetch | 1u << 19 | 0x24u << 26;
  const uint32_t fetch_word_1 = 0x688 | kUseFetchConstFilter << 12 | kUseFetchConstFilter << 14 |
                                kUseFetchConstFilter << 16 | kUseFetchConstAniso << 18 |
                                kUseFetchConstFilter << 24 | kUseFetchConstFilter << 26 | 1u << 28;
  const uint32_t fetch_word_2 = uint32_t(register_gradients) | kDimension2D << 14;
  return {exec_word_0, exec_word_1, 0, fetch_word_0, fetch_word_1, fetch_word_2};
}

struct Samples {
  uint32_t implicit_with_bias = 0;
  uint32_t implicit_other = 0;
  uint32_t explicit_with_gradients = 0;
  uint32_t explicit_other = 0;
};

Samples Translate(bool register_gradients, bool implicit_lod) {
  const std::vector<uint32_t> ucode = TextureFetchShader(register_gradients);
  SpirvShader shader(ShaderType::kPixel, 1, ucode.data(), ucode.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), false, false, false);
  SpirvShaderTranslator::Modification modification(translator.GetDefaultPixelShaderModification(0));
  modification.pixel.interpolator_mask = 1;
  modification.pixel.implicit_lod = implicit_lod;
  auto* translation = shader.GetOrCreateTranslation(modification.value);
  Samples samples;
  if (!translator.TranslateAnalyzedShader(*translation)) {
    std::fprintf(stderr, "translation failed\n");
    return samples;
  }
  const std::vector<uint8_t>& binary = translation->translated_binary();
  std::vector<uint32_t> words(binary.size() / 4);
  std::memcpy(words.data(), binary.data(), words.size() * 4);
  for (size_t at = 5; at < words.size(); at += words[at] >> 16) {
    const uint32_t op = words[at] & 0xFFFF;
    const uint32_t count = words[at] >> 16;
    const uint32_t operands = count > 5 ? words[at + 5] : 0;
    if (op == spv::OpImageSampleImplicitLod) {
      ++(operands & spv::ImageOperandsBiasMask ? samples.implicit_with_bias : samples.implicit_other);
    } else if (op == spv::OpImageSampleExplicitLod) {
      ++(operands & spv::ImageOperandsGradMask ? samples.explicit_with_gradients : samples.explicit_other);
    }
  }
  return samples;
}

}  // namespace

int main() {
  const Samples gradients = Translate(false, false);
  Check(gradients.explicit_with_gradients > 0 && !gradients.implicit_with_bias && !gradients.implicit_other,
        "computed_lod_samples_with_explicit_gradients_by_default");

  const Samples implicit = Translate(false, true);
  Check(implicit.implicit_with_bias > 0 && !implicit.explicit_with_gradients && !implicit.explicit_other &&
            !implicit.implicit_other,
        "implicit_lod_samples_with_a_bias_instead_of_gradients");
  Check(implicit.implicit_with_bias == gradients.explicit_with_gradients, "implicit_lod_keeps_every_sample");

  const Samples register_gradients = Translate(true, true);
  Check(register_gradients.explicit_with_gradients > 0 && !register_gradients.implicit_with_bias,
        "register_gradients_stay_explicit");
  return failures ? 1 : 0;
}
