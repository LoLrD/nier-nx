#include "gpu_registers.h"

#include <algorithm>
#include <atomic>

#include <rex/graphics/registers.h>

namespace nier::native {

using namespace rex::graphics;

GpuRegisters::GpuRegisters() {
  for (uint32_t i = 0; i < gamma_ramp_.size(); ++i) {
    const uint32_t value = i * 0x3FF / 0xFF;
    gamma_ramp_[i] = value << 20 | value << 10 | value;
  }
}

void GpuRegisters::WriteGammaComponent(uint32_t value) {
  const uint32_t index = file_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
  if (file_.values[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & (1u << (2 - gamma_component_))) {
    const uint32_t shift = 20 - gamma_component_ * 10;
    gamma_ramp_[index] = (gamma_ramp_[index] & ~(0x3FFu << shift)) | (((value >> 6) & 0x3FF) << shift);
    ++gamma_version_;
  }
  if (++gamma_component_ >= 3) {
    AdvanceGammaIndex(index);
  }
}

void GpuRegisters::WriteGammaEntry(uint32_t value) {
  const uint32_t index = file_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
  const uint32_t mask = file_.values[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 7;
  uint32_t keep = 0;
  for (uint32_t component = 0; component < 3; ++component) {
    if (!(mask & (1u << component))) {
      keep |= 0x3FFu << (component * 10);
    }
  }
  if (mask) {
    gamma_ramp_[index] = (gamma_ramp_[index] & keep) | (value & 0x3FFFFFFF & ~keep);
    ++gamma_version_;
  }
  AdvanceGammaIndex(index);
}

void GpuRegisters::AdvanceGammaIndex(uint32_t index) {
  gamma_component_ = 0;
  file_.values[XE_GPU_REG_DC_LUT_RW_INDEX] = (file_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & ~0xFFu) | ((index + 1) & 0xFF);
}

uint32_t GpuRegisters::Get(uint32_t index) const {
  return index < kCount ? file_.values[index] : 0;
}

std::optional<MemoryStore> GpuRegisters::Set(uint32_t index, uint32_t value) {
  if (index >= kCount) {
    return std::nullopt;
  }
  if (file_.values[index] != value && AffectsState(index)) {
    JournalState(index);
  }
  file_.values[index] = value;
  Touch(index);
  if (index == XE_GPU_REG_DC_LUT_RW_INDEX) {
    gamma_component_ = 0;
  } else if (index == XE_GPU_REG_DC_LUT_SEQ_COLOR) {
    WriteGammaComponent(value);
  } else if (index == XE_GPU_REG_DC_LUT_30_COLOR) {
    WriteGammaEntry(value);
  }
  if (index < XE_GPU_REG_SCRATCH_REG0 || index > XE_GPU_REG_SCRATCH_REG7) {
    return std::nullopt;
  }
  const uint32_t scratch = index - XE_GPU_REG_SCRATCH_REG0;
  if (!((1u << scratch) & file_.values[XE_GPU_REG_SCRATCH_UMSK])) {
    return std::nullopt;
  }
  return MemoryStore{file_.values[XE_GPU_REG_SCRATCH_ADDR] + scratch * 4, value};
}

bool GpuRegisters::AffectsState(uint32_t index) {
  if (index >= 0x2000 && index < kConstantBase) {
    switch (index) {
      case XE_GPU_REG_VGT_DRAW_INITIATOR:
      case XE_GPU_REG_VGT_DMA_BASE:
      case XE_GPU_REG_VGT_DMA_SIZE:
      case XE_GPU_REG_VGT_EVENT_INITIATOR:
      case XE_GPU_REG_VGT_IMMED_DATA:
      case XE_GPU_REG_VGT_INDX_OFFSET:
      case XE_GPU_REG_VGT_MIN_VTX_INDX:
      case XE_GPU_REG_VGT_MAX_VTX_INDX:
        return false;
      default:
        return index < XE_GPU_REG_COHER_DEST_BASE_0 || index > XE_GPU_REG_COHER_DEST_BASE_7;
    }
  }
  return index > XE_GPU_REG_SHADER_CONSTANT_LOOP_31;
}

void GpuRegisters::JournalState(uint32_t index) {
  uint64_t& word = journaled_[index >> 6];
  const uint64_t bit = uint64_t(1) << (index & 63);
  if (!(word & bit)) {
    word |= bit;
    state_journal_.emplace_back(index, file_.values[index]);
  }
}

void GpuRegisters::BeginStateJournal() const {
  for (const auto& [index, value] : state_journal_) {
    journaled_[index >> 6] &= ~(uint64_t(1) << (index & 63));
  }
  state_journal_.clear();
}

bool GpuRegisters::StateUnchanged() const {
  for (const auto& [index, value] : state_journal_) {
    if (file_.values[index] != value) {
      return false;
    }
  }
  return true;
}

bool GpuRegisters::SetRange(uint32_t first, const uint32_t* words_big_endian, uint32_t count) {
  if (first < kConstantBase || first + count > kCount) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = __builtin_bswap32(words_big_endian[i]);
    if (first + i > XE_GPU_REG_SHADER_CONSTANT_LOOP_31 && file_.values[first + i] != value) {
      JournalState(first + i);
    }
    file_.values[first + i] = value;
  }
  if (count) {
    const uint32_t last_group = (first + count - 1 - kConstantBase) >> kGroupShift;
    for (uint32_t group = (first - kConstantBase) >> kGroupShift; group <= last_group; ++group) {
      generations_[group] = ++generation_;
    }
  }
  return true;
}

void GpuRegisters::Store(uint32_t index, uint32_t value) {
  if (index < kCount) {
    if (std::atomic_ref<uint32_t>(file_.values[index]).exchange(value, std::memory_order_relaxed) != value &&
        AffectsState(index)) {
      foreign_state_writes_.fetch_add(1, std::memory_order_relaxed);
    }
    Touch(index);
  }
}

uint64_t GpuRegisters::FloatGeneration(const uint64_t (&bitmap)[4], bool pixel) const {
  const uint32_t first_group = pixel ? 1024 >> kGroupShift : 0;
  uint64_t newest = 0;
  for (uint32_t block = 0; block < 4; ++block) {
    for (uint32_t part = 0; part < 4; ++part) {
      if ((bitmap[block] >> (part * 16)) & 0xFFFF) {
        newest = std::max(newest, generations_[first_group + block * 4 + part]);
      }
    }
  }
  return newest;
}

uint64_t GpuRegisters::ConstantsGeneration(uint32_t first, uint32_t count) const {
  uint64_t newest = 0;
  for (uint32_t group = (first - kConstantBase) >> kGroupShift; group <= (first + count - 1 - kConstantBase) >> kGroupShift;
       ++group) {
    newest = std::max(newest, generations_[group]);
  }
  return newest;
}

uint32_t GpuRegisters::ReadMmio(uint32_t index, uint32_t display_width, uint32_t display_height) const {
  const uint32_t width = std::min(display_width, uint32_t(0x0FFF));
  const uint32_t height = std::min(display_height, uint32_t(0x0FFF));
  switch (index) {
    case 0x0F00:
      return 0x08100748;
    case 0x0F01:
      return 0x0000200E;
    case 0x194C:
      return height;
    case 0x1951:
      return 1;
    case 0x1961:
      return (width << 16) | height;
    default:
      return Get(index);
  }
}

}  // namespace nier::native
