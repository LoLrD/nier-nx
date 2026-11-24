#include "pm4_executor.h"

#include <chrono>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace nier::native {

namespace {

using namespace rex::graphics;

constexpr uint32_t kQueryFinished = 0xEDFEFFFFu;
constexpr uint32_t kFakeSampleCount = 1000;
constexpr uint32_t kCopyEdramMode = 6;

uint32_t ConstantBase(uint32_t type) {
  switch (type) {
    case 0:
      return XE_GPU_REG_SHADER_CONSTANT_000_X;
    case 1:
      return XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    case 2:
      return XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
    case 3:
      return XE_GPU_REG_SHADER_CONSTANT_LOOP_00;
    case 4:
      return 0x2000;
    default:
      return UINT32_MAX;
  }
}

}  // namespace

Pm4Executor::Pm4Executor(GpuRegisters& registers, GpuBus& bus) : registers_(registers), bus_(bus) {}

Pm4Executor::Result Pm4Executor::Execute(RingReader& reader, uint32_t publish_step) {
  const uint32_t size = reader.size_words();
  uint32_t published = reader.position();
  while (reader.Pending()) {
    const Pm4Header header = DecodePm4(reader.Peek());
    if (size && header.count + 1 > size) {
      ++stats_.overflows;
      reader.Skip(reader.Pending());
      break;
    }
    if (reader.Pending() < header.count + 1) {
      return Result::kPartial;
    }
    reader.Skip(1);
    RingReader data = reader;
    reader.Skip(header.count);
    ++stats_.packets;
    switch (header.type) {
      case 0:
        if (header.single_register) {
          for (uint32_t i = 0; i < header.count; ++i) {
            SetRegister(header.base_index, data.Read());
          }
        } else {
          SetRegisters(header.base_index, data, header.count);
        }
        break;
      case 1:
        SetRegister(header.base_index, data.Read());
        SetRegister(header.opcode, data.Read());
        break;
      case 2:
        break;
      default:
        if (!Type3(data, header.opcode, header.count)) {
          return Result::kAborted;
        }
        break;
    }
    if (publish_step && ((reader.position() - published) & (size - 1)) >= publish_step) {
      published = reader.position();
      bus_.PublishReadPointer(published);
    }
  }
  return Result::kDone;
}

bool Pm4Executor::Type3(RingReader& data, uint32_t opcode, uint32_t count) {
  switch (opcode) {
    case xenos::PM4_INTERRUPT: {
      const uint32_t cpu_mask = data.Read();
      for (uint32_t n = 0; n < 6; ++n) {
        if (cpu_mask & (1u << n)) {
          bus_.Interrupt(1, n);
          ++stats_.interrupts;
        }
      }
      return true;
    }
    case xenos::PM4_XE_SWAP: {
      data.Read();
      const uint32_t frontbuffer = data.Read();
      const uint32_t width = data.Read();
      const uint32_t height = data.Read();
      ++stats_.swaps;
      IncrementCounter();
      bus_.Swap(frontbuffer, width, height);
      return true;
    }
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      const uint32_t address = xenos::GpuToCpu(xenos::CpuToGpu(data.Read()));
      const uint32_t length = data.Read() & 0xFFFFF;
      RingReader buffer(bus_.Words(address), 0, 0, length);
      return Execute(buffer) != Result::kAborted;
    }
    case xenos::PM4_WAIT_REG_MEM:
      return WaitRegMem(data);
    case xenos::PM4_REG_RMW: {
      const uint32_t info = data.Read();
      const uint32_t and_mask = data.Read();
      const uint32_t or_mask = data.Read();
      uint32_t value = registers_.Get(info & 0x1FFF);
      value &= (info >> 31) & 1 ? registers_.Get(and_mask & 0x1FFF) : and_mask;
      value |= (info >> 30) & 1 ? registers_.Get(or_mask & 0x1FFF) : or_mask;
      SetRegister(info & 0x1FFF, value);
      return true;
    }
    case xenos::PM4_REG_TO_MEM: {
      const uint32_t index = data.Read();
      const uint32_t address = data.Read();
      StoreSwapped(address, registers_.Get(index));
      return true;
    }
    case xenos::PM4_MEM_WRITE: {
      uint32_t address = data.Read();
      for (uint32_t i = 1; i < count; ++i, address += 4) {
        StoreSwapped(address, data.Read());
      }
      return true;
    }
    case xenos::PM4_COND_WRITE:
      CondWrite(data);
      return true;
    case xenos::PM4_EVENT_WRITE:
      SetRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
      return true;
    case xenos::PM4_EVENT_WRITE_SHD: {
      const uint32_t initiator = data.Read();
      const uint32_t address = data.Read();
      const uint32_t value = data.Read();
      SetRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      StoreSwapped(address, (initiator >> 31) & 1 ? counter_.load(std::memory_order_relaxed) : value);
      return true;
    }
    case xenos::PM4_EVENT_WRITE_EXT:
      EventWriteExt(data);
      return true;
    case xenos::PM4_EVENT_WRITE_ZPD:
      EventWriteZpd(data);
      return true;
    case xenos::PM4_DRAW_INDX:
      Draw(data, true, count);
      return true;
    case xenos::PM4_DRAW_INDX_2:
      Draw(data, false, count);
      return true;
    case xenos::PM4_SET_CONSTANT:
      SetConstant(data, count);
      return true;
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS: {
      const uint32_t index = data.Read() & 0xFFFF;
      for (uint32_t i = 1; i < count; ++i) {
        SetRegister(index + i - 1, data.Read());
      }
      return true;
    }
    case xenos::PM4_LOAD_ALU_CONSTANT:
      LoadAluConstant(data);
      return true;
    case xenos::PM4_IM_LOAD: {
      const uint32_t address_type = data.Read();
      const uint32_t size = data.Read() & 0xFFFF;
      ++stats_.shader_loads;
      bus_.ShaderLoad(address_type & 3, address_type & ~3u, bus_.Words(address_type & ~3u), size);
      return true;
    }
    case xenos::PM4_IM_LOAD_IMMEDIATE:
      ImLoadImmediate(data);
      return true;
    case xenos::PM4_VIZ_QUERY:
      VizQuery(data);
      return true;
    default:
      return true;
  }
}

void Pm4Executor::SetRegister(uint32_t index, uint32_t value) {
  if (index == XE_GPU_REG_COHER_STATUS_HOST) {
    value |= 0x80000000u;
  }
  if (index == XE_GPU_REG_COHER_BASE_HOST) {
    ++stats_.coher_events;
    stats_.coher_bytes += registers_.Get(XE_GPU_REG_COHER_SIZE_HOST);
  }
  if (const auto store = registers_.Set(index, value)) {
    bus_.StoreWord(store->address & ~3u, __builtin_bswap32(store->value));
  }
}

void Pm4Executor::SetRegisters(uint32_t first, RingReader& data, uint32_t count) {
  if (const uint32_t* words = data.Contiguous(count); words && registers_.SetRange(first, words, count)) {
    data.Skip(count);
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    SetRegister(first + i, data.Read());
  }
}

uint32_t Pm4Executor::LoadSwapped(uint32_t address) {
  return xenos::GpuSwap(bus_.LoadWord(address & ~3u), static_cast<xenos::Endian>(address & 3));
}

void Pm4Executor::StoreSwapped(uint32_t address, uint32_t value) {
  bus_.StoreWord(address & ~3u, xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 3)));
}

bool Pm4Executor::Compare(uint32_t function, uint32_t value, uint32_t mask, uint32_t reference) const {
  const uint32_t masked = value & mask;
  switch (function & 7) {
    case 1:
      return masked < reference;
    case 2:
      return masked <= reference;
    case 3:
      return masked == reference;
    case 4:
      return masked != reference;
    case 5:
      return masked >= reference;
    case 6:
      return masked > reference;
    case 7:
      return true;
    default:
      return false;
  }
}

bool Pm4Executor::WaitRegMem(RingReader& data) {
  const uint32_t info = data.Read();
  const uint32_t poll = data.Read();
  const uint32_t reference = data.Read();
  const uint32_t mask = data.Read();
  const uint32_t wait = data.Read();
  ++stats_.waits;
  std::chrono::steady_clock::time_point start{};
  while (true) {
    uint32_t value;
    if (info & 0x10) {
      value = LoadSwapped(poll);
    } else {
      if (poll == XE_GPU_REG_COHER_STATUS_HOST && (registers_.Get(poll) & 0x80000000u)) {
        registers_.Set(poll, 0);
      }
      value = registers_.Get(poll);
    }
    if (Compare(info, value, mask, reference)) {
      if (start != std::chrono::steady_clock::time_point{}) {
        CountSlowWait(start, info, poll, reference, mask, value);
      }
      return true;
    }
    if (start == std::chrono::steady_clock::time_point{}) {
      start = std::chrono::steady_clock::now();
    }
    if (!bus_.Yield(wait)) {
      return false;
    }
  }
}

void Pm4Executor::CountSlowWait(std::chrono::steady_clock::time_point start, uint32_t info, uint32_t poll,
                                uint32_t reference, uint32_t mask, uint32_t value) {
  ++stats_.slow_waits;
  stats_.slow_wait_us += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  stats_.slow_wait_info = info;
  stats_.slow_wait_poll = poll;
  stats_.slow_wait_reference = reference;
  stats_.slow_wait_mask = mask;
  stats_.slow_wait_value = value;
}

void Pm4Executor::CondWrite(RingReader& data) {
  const uint32_t info = data.Read();
  const uint32_t poll = data.Read();
  const uint32_t reference = data.Read();
  const uint32_t mask = data.Read();
  const uint32_t target = data.Read();
  const uint32_t value = data.Read();
  const uint32_t current = info & 0x10 ? LoadSwapped(poll) : registers_.Get(poll);
  if (!Compare(info, current, mask, reference)) {
    return;
  }
  if (info & 0x100) {
    StoreSwapped(target, value);
  } else {
    SetRegister(target, value);
  }
}

void Pm4Executor::EventWriteExt(RingReader& data) {
  const uint32_t initiator = data.Read();
  const uint32_t address = data.Read() & ~3u;
  SetRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  const uint16_t extents[6] = {0, uint16_t(xenos::kTexture2DCubeMaxWidthHeight >> 3), 0,
                               uint16_t(xenos::kTexture2DCubeMaxWidthHeight >> 3), 0, 1};
  for (uint32_t i = 0; i < 3; ++i) {
    bus_.StoreWord(address + i * 4, uint32_t(__builtin_bswap16(extents[i * 2])) |
                                        (uint32_t(__builtin_bswap16(extents[i * 2 + 1])) << 16));
  }
}

void Pm4Executor::EventWriteZpd(RingReader& data) {
  SetRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
  ++stats_.zpd_events;
  const uint32_t address = registers_.Get(XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
  if (!address) {
    return;
  }
  bool finished = false;
  for (uint32_t i = 2; i < 6; ++i) {
    finished |= bus_.LoadWord(address + i * 4) == kQueryFinished;
  }
  for (uint32_t i = 0; i < 8; ++i) {
    bus_.StoreWord(address + i * 4, 0);
  }
  if (finished) {
    ++stats_.zpd_results;
    bus_.StoreWord(address, kFakeSampleCount);
    bus_.StoreWord(address + 16, kFakeSampleCount);
  }
}

void Pm4Executor::SetConstant(RingReader& data, uint32_t count) {
  const uint32_t offset_type = data.Read();
  const uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
  if (base == UINT32_MAX) {
    return;
  }
  SetRegisters(base + (offset_type & 0x7FF), data, count - 1);
}

void Pm4Executor::LoadAluConstant(RingReader& data) {
  const uint32_t address = data.Read() & 0x3FFFFFFF;
  const uint32_t offset_type = data.Read();
  const uint32_t size = data.Read() & 0xFFF;
  const uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
  if (base == UINT32_MAX) {
    return;
  }
  RingReader source(bus_.Words(address), 0, 0, size);
  SetRegisters(base + (offset_type & 0x7FF), source, size);
}

void Pm4Executor::Draw(RingReader& data, bool has_viz_query, uint32_t count) {
  if (has_viz_query) {
    data.Read();
    --count;
  }
  const uint32_t initiator = data.Read();
  SetRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
  if (((initiator >> 6) & 3) == uint32_t(xenos::SourceSelect::kDMA) && count >= 3) {
    SetRegister(XE_GPU_REG_VGT_DMA_BASE, data.Read());
    SetRegister(XE_GPU_REG_VGT_DMA_SIZE, data.Read());
  }
  const bool resolve = (registers_.Get(XE_GPU_REG_RB_MODECONTROL) & 7) == kCopyEdramMode;
  ++(resolve ? stats_.resolves : stats_.draws);
  bus_.Draw(resolve);
}

void Pm4Executor::ImLoadImmediate(RingReader& data) {
  const uint32_t type = data.Read();
  const uint32_t size = data.Read() & 0xFFFF;
  ++stats_.shader_loads;
  if (const uint32_t* direct = data.Contiguous(size)) {
    data.Skip(size);
    bus_.ShaderLoad(type, 0, direct, size);
    return;
  }
  inline_microcode_.resize(size);
  for (uint32_t i = 0; i < size; ++i) {
    inline_microcode_[i] = __builtin_bswap32(data.Read());
  }
  bus_.ShaderLoad(type, 0, inline_microcode_.data(), size);
}

void Pm4Executor::VizQuery(RingReader& data) {
  const uint32_t word = data.Read();
  const uint32_t id = word & 0x3F;
  if (!(word & 0x100)) {
    SetRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
    return;
  }
  SetRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
  const uint32_t status = id < 32 ? XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0 : XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1;
  SetRegister(status, registers_.Get(status) | (1u << (id & 31)));
}

}  // namespace nier::native
