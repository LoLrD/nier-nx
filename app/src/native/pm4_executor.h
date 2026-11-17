#pragma once

#include <atomic>
#include <cstdint>

#include "gpu_registers.h"
#include "pm4_reader.h"

namespace nier::native {

class GpuBus {
 public:
  virtual ~GpuBus() = default;
  virtual uint32_t LoadWord(uint32_t physical) = 0;
  virtual void StoreWord(uint32_t physical, uint32_t raw) = 0;
  virtual const uint32_t* Words(uint32_t physical) = 0;
  virtual void Interrupt(uint32_t source, uint32_t cpu) = 0;
  virtual void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height) = 0;
  virtual void PublishReadPointer(uint32_t position) = 0;
  virtual bool Yield(uint32_t wait) = 0;
};

struct Pm4Stats {
  uint64_t packets = 0;
  uint64_t draws = 0;
  uint64_t resolves = 0;
  uint64_t swaps = 0;
  uint64_t interrupts = 0;
  uint64_t shader_loads = 0;
  uint64_t waits = 0;
  uint64_t overflows = 0;
};

class Pm4Executor {
 public:
  enum class Result { kDone, kPartial, kAborted };

  Pm4Executor(GpuRegisters& registers, GpuBus& bus);

  Result Execute(RingReader& reader, uint32_t publish_step = 0);
  void IncrementCounter() { counter_.fetch_add(1, std::memory_order_relaxed); }
  const Pm4Stats& stats() const { return stats_; }

 private:
  bool Type3(RingReader& data, uint32_t opcode, uint32_t count);
  void SetRegister(uint32_t index, uint32_t value);
  uint32_t LoadSwapped(uint32_t address);
  void StoreSwapped(uint32_t address, uint32_t value);
  bool Compare(uint32_t function, uint32_t value, uint32_t mask, uint32_t reference) const;
  bool WaitRegMem(RingReader& data);
  void CondWrite(RingReader& data);
  void EventWriteExt(RingReader& data);
  void EventWriteZpd(RingReader& data);
  void SetConstant(RingReader& data, uint32_t count);
  void LoadAluConstant(RingReader& data);
  void Draw(RingReader& data, bool has_viz_query);
  void VizQuery(RingReader& data);

  GpuRegisters& registers_;
  GpuBus& bus_;
  Pm4Stats stats_;
  std::atomic<uint32_t> counter_{0};
};

}  // namespace nier::native
