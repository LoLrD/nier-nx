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
  static constexpr uint32_t kConstantBase = 0x4000;
  static constexpr uint32_t kGroupShift = 6;
  static constexpr uint32_t kGroups = (kCount - kConstantBase + (1u << kGroupShift) - 1) >> kGroupShift;

  uint32_t Get(uint32_t index) const;
  std::optional<MemoryStore> Set(uint32_t index, uint32_t value);
  void Store(uint32_t index, uint32_t value);
  uint32_t ReadMmio(uint32_t index, uint32_t display_width, uint32_t display_height) const;
  const rex::graphics::RegisterFile& file() const { return file_; }
  const std::array<uint32_t, 256>& gamma_ramp() const { return gamma_ramp_; }
  uint64_t gamma_version() const { return gamma_version_; }
  uint64_t FloatGeneration(const uint64_t (&bitmap)[4], bool pixel) const;
  uint64_t ConstantsGeneration(uint32_t first, uint32_t count) const;

  GpuRegisters();

 private:
  void WriteGammaComponent(uint32_t value);
  void WriteGammaEntry(uint32_t value);
  void AdvanceGammaIndex(uint32_t index);
  void Touch(uint32_t index) {
    if (index >= kConstantBase && index < kCount) {
      generations_[(index - kConstantBase) >> kGroupShift] = ++generation_;
    }
  }

  rex::graphics::RegisterFile file_;
  std::array<uint32_t, 256> gamma_ramp_;
  uint32_t gamma_component_ = 0;
  uint64_t gamma_version_ = 0;
  std::array<uint64_t, kGroups> generations_{};
  uint64_t generation_ = 0;
};

}  // namespace nier::native
