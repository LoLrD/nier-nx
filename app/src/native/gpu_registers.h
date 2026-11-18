#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <rex/graphics/register_file.h>

namespace nier::native {

struct MemoryStore {
  uint32_t address;
  uint32_t value;
};

class GpuRegisters {
 public:
  static constexpr uint32_t kCount = 0x5003;

  uint32_t Get(uint32_t index) const;
  std::optional<MemoryStore> Set(uint32_t index, uint32_t value);
  void Store(uint32_t index, uint32_t value);
  uint32_t ReadMmio(uint32_t index, uint32_t display_width, uint32_t display_height) const;
  const rex::graphics::RegisterFile& file() const { return file_; }
  const std::array<uint32_t, 256>& gamma_ramp() const { return gamma_ramp_; }
  uint64_t gamma_version() const { return gamma_version_; }

  GpuRegisters();

 private:
  void WriteGammaComponent(uint32_t value);
  void WriteGammaEntry(uint32_t value);
  void AdvanceGammaIndex(uint32_t index);

  rex::graphics::RegisterFile file_;
  std::array<uint32_t, 256> gamma_ramp_;
  uint32_t gamma_component_ = 0;
  uint64_t gamma_version_ = 0;
};

}  // namespace nier::native
