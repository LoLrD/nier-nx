#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include "pm4_executor.h"

using namespace nier::native;
using namespace rex::graphics;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

class FakeBus final : public GpuBus {
 public:
  std::unordered_map<uint32_t, uint32_t> memory;
  std::unordered_map<uint32_t, std::vector<uint32_t>> buffers;
  std::vector<std::pair<uint32_t, uint32_t>> interrupts;
  std::vector<std::vector<uint32_t>> swaps;
  std::vector<uint32_t> published;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> shader_loads;
  std::vector<bool> draws;
  uint32_t yields = 0;
  uint32_t release_after = 0;
  uint32_t release_address = 0;
  uint32_t release_raw = 0;
  bool stopping = false;

  uint32_t LoadWord(uint32_t physical) override { return memory[physical]; }
  void StoreWord(uint32_t physical, uint32_t raw) override { memory[physical] = raw; }
  const uint32_t* Words(uint32_t physical) override { return buffers[physical].data(); }
  void Interrupt(uint32_t source, uint32_t cpu) override { interrupts.emplace_back(source, cpu); }
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height) override {
    swaps.push_back({frontbuffer, width, height});
  }
  void PublishReadPointer(uint32_t position) override { published.push_back(position); }
  void Draw(bool resolve) override { draws.push_back(resolve); }
  void ShaderLoad(uint32_t type, uint32_t, const uint32_t* words, uint32_t size) override {
    shader_loads.emplace_back(type, std::vector<uint32_t>(words, words + size));
  }
  bool Yield(uint32_t) override {
    ++yields;
    if (release_after && yields == release_after) memory[release_address] = release_raw;
    return !stopping;
  }
};

static uint32_t Be(uint32_t value) {
  return __builtin_bswap32(value);
}

static uint32_t Type3(uint32_t opcode, uint32_t count) {
  return 0xC0000000u | ((count - 1) << 16) | (opcode << 8);
}

struct Stream {
  std::vector<uint32_t> words;
  Stream& operator<<(uint32_t value) {
    words.push_back(Be(value));
    return *this;
  }
  RingReader Reader() const { return RingReader(words.data(), 0, 0, uint32_t(words.size())); }
};

int main() {
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << 0x00012000 << 11 << 22;
    auto reader = s.Reader();
    Check(exec.Execute(reader) == Pm4Executor::Result::kDone, "type0 result");
    Check(regs.Get(0x2000) == 11 && regs.Get(0x2001) == 22, "type0_sets_registers");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    regs.Set(XE_GPU_REG_SCRATCH_UMSK, 1);
    regs.Set(XE_GPU_REG_SCRATCH_ADDR, 0x3000);
    Stream s;
    s << uint32_t(XE_GPU_REG_SCRATCH_REG0) << 0x12345678;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.memory[0x3000] == Be(0x12345678), "scratch_write_reaches_memory");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_INTERRUPT, 1) << 0b101;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.interrupts.size() == 2 && bus.interrupts[0] == std::make_pair(1u, 0u) &&
              bus.interrupts[1] == std::make_pair(1u, 2u) && exec.stats().interrupts == 2,
          "interrupt_per_cpu_bit");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_XE_SWAP, 4) << xenos::kSwapSignature << 0x1F000000 << 1280 << 720;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.swaps.size() == 1 && bus.swaps[0] == std::vector<uint32_t>{0x1F000000, 1280, 720} &&
              exec.stats().swaps == 1,
          "swap_reports_frontbuffer");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_MEM_WRITE, 3) << (0x4000 | 2) << 0xAABBCCDD << 0x11223344;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.memory[0x4000] == Be(0xAABBCCDD) && bus.memory[0x4004] == Be(0x11223344),
          "mem_write_stores_swapped");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_XE_SWAP, 4) << xenos::kSwapSignature << 0 << 1280 << 720;
    s << Type3(xenos::PM4_XE_SWAP, 4) << xenos::kSwapSignature << 0 << 1280 << 720;
    s << Type3(xenos::PM4_EVENT_WRITE_SHD, 3) << 0x80000000u << (0x5000 | 2) << 99;
    s << Type3(xenos::PM4_EVENT_WRITE_SHD, 3) << 0 << (0x5004 | 2) << 99;
    auto reader = s.Reader();
    exec.IncrementCounter();
    exec.Execute(reader);
    Check(bus.memory[0x5000] == Be(3), "event_write_shd_counter counts swaps and vblanks");
    Check(bus.memory[0x5004] == Be(99), "event_write_shd_value");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    bus.release_after = 3;
    bus.release_address = 0x6000;
    bus.release_raw = Be(5);
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_WAIT_REG_MEM, 5) << (0x10 | 0x3) << (0x6000 | 2) << 5 << 0xFFFFFFFF << 0x100;
    auto reader = s.Reader();
    Check(exec.Execute(reader) == Pm4Executor::Result::kDone && bus.yields == 3,
          "wait_reg_mem_polls_memory");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    bus.stopping = true;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_WAIT_REG_MEM, 5) << (0x10 | 0x3) << (0x6000 | 2) << 5 << 0xFFFFFFFF << 0x100;
    auto reader = s.Reader();
    Check(exec.Execute(reader) == Pm4Executor::Result::kAborted, "wait_aborts_on_shutdown");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << uint32_t(XE_GPU_REG_COHER_STATUS_HOST) << 0x03000000u;
    s << Type3(xenos::PM4_WAIT_REG_MEM, 5) << 0x3 << uint32_t(XE_GPU_REG_COHER_STATUS_HOST) << 0 << 0xFFFFFFFFu
      << 0x100;
    auto reader = s.Reader();
    Check(exec.Execute(reader) == Pm4Executor::Result::kDone && bus.yields == 0 &&
              regs.Get(XE_GPU_REG_COHER_STATUS_HOST) == 0,
          "wait_on_coher_status_makes_coherent");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    bus.buffers[0x7000] = {Be(Type3(xenos::PM4_MEM_WRITE, 2)), Be(0x8000 | 2), Be(42)};
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_INDIRECT_BUFFER, 2) << 0x7000 << 3;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.memory[0x8000] == Be(42), "indirect_buffer_runs_nested");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_MEM_WRITE, 3) << (0x4000 | 2) << 1;
    auto reader = s.Reader();
    Check(exec.Execute(reader) == Pm4Executor::Result::kPartial && reader.position() == 0,
          "partial_packet_waits");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_DRAW_INDX, 2) << 0 << 0x00030004;
    regs.Set(XE_GPU_REG_RB_MODECONTROL, 6);
    s << Type3(xenos::PM4_DRAW_INDX_2, 1) << 0x00030004;
    auto reader = s.Reader();
    regs.Set(XE_GPU_REG_RB_MODECONTROL, 4);
    exec.Execute(reader);
    Check(exec.stats().draws == 2 && exec.stats().resolves == 0, "draw_counted");
    regs.Set(XE_GPU_REG_RB_MODECONTROL, 6);
    Stream copy;
    copy << Type3(xenos::PM4_DRAW_INDX_2, 1) << 0x00030004;
    auto copy_reader = copy.Reader();
    exec.Execute(copy_reader);
    Check(exec.stats().resolves == 1 && exec.stats().draws == 2, "copy_counted_as_resolve");
    Check(bus.draws == std::vector<bool>{false, false, true}, "draw_reports_to_bus");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    regs.Set(XE_GPU_REG_RB_MODECONTROL, 4);
    Stream s;
    s << Type3(xenos::PM4_DRAW_INDX, 4) << 0 << 0x00030004 << 0x01234560 << 0x40000010;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(regs.Get(XE_GPU_REG_VGT_DMA_BASE) == 0x01234560 && regs.Get(XE_GPU_REG_VGT_DMA_SIZE) == 0x40000010,
          "draw_indx_sets_index_buffer");
    Stream s2;
    s2 << Type3(xenos::PM4_DRAW_INDX_2, 3) << 0x00050004 << 0x02000000 << 0x00000020;
    auto reader2 = s2.Reader();
    exec.Execute(reader2);
    Check(regs.Get(XE_GPU_REG_VGT_DMA_BASE) == 0x02000000 && regs.Get(XE_GPU_REG_VGT_DMA_SIZE) == 0x00000020,
          "draw_indx_2_sets_index_buffer");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_SET_CONSTANT, 3) << ((0u << 16) | 4) << 0x3F800000 << 0x40000000;
    s << Type3(xenos::PM4_SET_CONSTANT, 2) << ((4u << 16) | 0x10) << 77;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(regs.Get(0x4004) == 0x3F800000 && regs.Get(0x4005) == 0x40000000, "set_constant_alu");
    Check(regs.Get(0x2010) == 77, "set_constant_registers");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    regs.Set(XE_GPU_REG_RB_SAMPLE_COUNT_ADDR, 0x9000);
    bus.memory[0x9000 + 16] = 0xEDFEFFFFu;
    bus.memory[0x9000 + 4] = 123;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_EVENT_WRITE_ZPD, 1) << 0x15;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.memory[0x9000] == 1000 && bus.memory[0x9000 + 16] == 1000 && bus.memory[0x9000 + 4] == 0,
          "zpd_fake_sample_count");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << uint32_t(XE_GPU_REG_COHER_STATUS_HOST) << 0x03000000u;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(regs.Get(XE_GPU_REG_COHER_STATUS_HOST) == 0x83000000u, "ring_write_marks_coher_status_dirty");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    std::vector<uint32_t> ring(64);
    uint32_t w = 0;
    for (uint32_t i = 0; i < 12; ++i) {
      ring[w++] = Be(Type3(xenos::PM4_NOP, 3));
      w += 3;
    }
    RingReader reader(ring.data(), 64, 0, w);
    exec.Execute(reader, 0x20);
    Check(bus.published == std::vector<uint32_t>{32}, "read_pointer_published_while_executing");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    std::vector<uint32_t> ring(8);
    ring[0] = Be(Type3(xenos::PM4_NOP, 100));
    RingReader reader(ring.data(), 8, 0, 4);
    Check(exec.Execute(reader) == Pm4Executor::Result::kDone && reader.position() == 4 && exec.stats().overflows == 1,
          "packet_larger_than_ring_is_dropped");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    std::vector<uint32_t> ring(8);
    ring[6] = Be(Type3(xenos::PM4_MEM_WRITE, 2));
    ring[7] = Be(0x4000 | 2);
    ring[0] = Be(55);
    RingReader reader(ring.data(), 8, 6, 1);
    exec.Execute(reader);
    Check(bus.memory[0x4000] == Be(55) && reader.position() == 1, "packet_wrapping_across_ring_end");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    std::vector<uint32_t> microcode(24);
    for (uint32_t i = 0; i < 24; ++i) microcode[i] = Be(0x1000 + i);
    bus.buffers[0xA000] = microcode;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_IM_LOAD, 2) << (0xA000 | 1) << 24;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(bus.shader_loads.size() == 1 && bus.shader_loads[0].first == 1 && bus.shader_loads[0].second == microcode &&
              exec.stats().shader_loads == 1,
          "im_load_reports_words");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << Type3(xenos::PM4_IM_LOAD_IMMEDIATE, 8) << 0 << 6;
    for (uint32_t i = 0; i < 6; ++i) s << (0x2000 + i);
    auto reader = s.Reader();
    exec.Execute(reader);
    std::vector<uint32_t> expected;
    for (uint32_t i = 0; i < 6; ++i) expected.push_back(Be(0x2000 + i));
    Check(bus.shader_loads.size() == 1 && bus.shader_loads[0].first == 0 && bus.shader_loads[0].second == expected,
          "im_load_immediate_reports_inline_words");
  }
  {
    GpuRegisters regs;
    FakeBus bus;
    Pm4Executor exec(regs, bus);
    Stream s;
    s << uint32_t(XE_GPU_REG_COHER_SIZE_HOST) << 0x2000u;
    s << uint32_t(XE_GPU_REG_COHER_BASE_HOST) << 0x100u;
    s << uint32_t(XE_GPU_REG_COHER_STATUS_HOST) << 0x1u;
    s << uint32_t(XE_GPU_REG_COHER_SIZE_HOST) << 0x40u;
    auto reader = s.Reader();
    exec.Execute(reader);
    Check(exec.stats().coher_events == 1 && exec.stats().coher_bytes == 0x2000u, "coher_base_write_counted_with_size_in_bytes");
  }
  return failures ? 1 : 0;
}
