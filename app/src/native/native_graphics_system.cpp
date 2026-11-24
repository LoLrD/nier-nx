#include "native_graphics_system.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <time.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#include "gpu_progress.h"
#include "gpu_registers.h"
#include "perf_modes.h"
#include "pm4_executor.h"
#include "pm4_reader.h"
#include "ring_state.h"
#include "native_renderer.h"
#include "native_shaders.h"
#include "nier_frame_pacing.h"
#include "nier_npc_lod.h"
#include "shader_dump.h"

REXCVAR_DEFINE_STRING(nier_renderer, "xenos", "NieR", "Graphics: xenos (SDK emulation) or native")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nier_frame_dump_dir, "", "NieR",
                      "Native renderer: write every nier_frame_dump_interval-th frame to this folder as PPM")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nier_frame_dump_interval, 120, "NieR", "Native renderer: frames between dumped frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nier_shader_dump_dir, "", "NieR",
                      "Native renderer: write each new shader microcode the game loads to this folder")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(double, nier_npc_update_scale);

namespace nier::native {

namespace {

using Clock = std::chrono::steady_clock;
using rex::X_STATUS;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr auto kRingWaitTimeout = std::chrono::milliseconds(5);
constexpr auto kReportInterval = std::chrono::seconds(10);
constexpr uint32_t kMinPublishStep = 0x20;
constexpr uint32_t kDefaultPublishStep = 0x100;
constexpr uint32_t kNoPartialPacket = UINT32_MAX;

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem, private GpuBus {
 public:
  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
    if (!provider_) {
      REXLOG_ERROR("[native] could not create the Vulkan device");
      return X_STATUS_UNSUCCESSFUL;
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] native graphics system");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &MmioRead, &MmioWrite)) {
      REXLOG_ERROR("[native] could not register the GPU MMIO range");
      return X_STATUS_UNSUCCESSFUL;
    }
    if (provider_) {
      const auto* device = static_cast<rex::ui::vulkan::VulkanProvider*>(provider_.get())->vulkan_device();
      shaders_ = std::make_unique<NativeShaders>(device);
      renderer_ = std::make_unique<NativeRenderer>(device, memory_, *shaders_, registers_);
      InstallPerfModes();
      if (const std::string dir = REXCVAR_GET(nier_frame_dump_dir); !dir.empty()) {
        renderer_->SetFrameDump(dir, uint32_t(std::max(1, REXCVAR_GET(nier_frame_dump_interval))));
      }
    }
    if (const std::string dir = REXCVAR_GET(nier_shader_dump_dir); !dir.empty()) {
      shader_dump_ = std::make_unique<ShaderDump>(dir);
      REXLOG_INFO("[native] shader discovery into {}", dir);
    }
    active_.store(true, std::memory_order_release);
    EnableGpuProgress(true);
    report_start_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync native");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring native");
    ring_thread_->Create();
    if (renderer_ && renderer_->recorder()) {
      VulkanRecorder* recorder = renderer_->recorder();
      record_thread_ = rex::system::object_ref<rex::system::XHostThread>(
          new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [recorder]() {
            recorder->Run();
            return 0;
          }));
      record_thread_->set_name("GPU record native");
      record_thread_->Create();
    }
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_.Initialize(ptr, size_log2);
    REXLOG_INFO("[native] ring at {:08X}, {} words", ptr, ring_.size_words());
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    const uint32_t frequency = (uint32_t(1) << block_size_log2) >> 2;
    publish_step_.store(frequency ? std::max(frequency, kMinPublishStep) : kDefaultPublishStep,
                        std::memory_order_release);
    read_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    if (active_.exchange(false, std::memory_order_acq_rel)) {
      EnableGpuProgress(false);
      { std::lock_guard<std::mutex> lock(ring_mutex_); }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (record_thread_) {
        SetActiveRecorder(nullptr);
        renderer_->recorder()->Stop();
        record_thread_->Wait(0, 0, 0, nullptr);
        record_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
    }
    renderer_.reset();
    shaders_.reset();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  static uint32_t MmioRead(void*, void* context, uint32_t address) {
    return static_cast<NativeGraphicsSystem*>(context)->ReadMmio(address);
  }

  static void MmioWrite(void*, void* context, uint32_t address, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(context)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    return registers_.ReadMmio((address & 0xFFFF) / 4, mode.display_width, mode.display_height);
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t index = (address & 0xFFFF) / 4;
    if (index == kRegCpRbWptr) {
      ring_.SetWritePointer(value);
      if (ring_waiting_.load(std::memory_order_seq_cst)) {
        { std::lock_guard<std::mutex> lock(ring_mutex_); }
        ring_cv_.notify_one();
      }
      return;
    }
    registers_.Store(index, value);
  }

  uint32_t LoadWord(uint32_t physical) override {
    uint32_t raw;
    std::memcpy(&raw, memory_->TranslatePhysical(physical), 4);
    return raw;
  }

  void StoreWord(uint32_t physical, uint32_t raw) override {
    std::memcpy(memory_->TranslatePhysical(physical), &raw, 4);
    memory_->MarkStoreCards(0xA0000000u + physical, 4);
    SharedGpuProgress().Advance();
  }

  const uint32_t* Words(uint32_t physical) override {
    return memory_->TranslatePhysical<const uint32_t*>(physical);
  }

  void Interrupt(uint32_t source, uint32_t cpu) override {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    thread->SetActiveCpu(cpu == 0xFFFFFFFF ? 2 : cpu);
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    SharedGpuProgress().Advance();
  }

  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height) override {
    REXLOG_DEBUG("[native] present {} {}x{}", executor_.stats().swaps, width, height);
    if (renderer_) {
      renderer_->Swap(presenter_.get(), frontbuffer, width, height);
    }
  }

  void PublishReadPointer(uint32_t position) override {
    if (const uint32_t writeback = read_writeback_.load(std::memory_order_acquire)) {
      StoreWord(writeback, __builtin_bswap32(position));
    }
  }

  void ShaderLoad(uint32_t type, uint32_t address, const uint32_t* words_big_endian, uint32_t size_words) override {
    if (shader_dump_) {
      shader_dump_->Record(type, address, words_big_endian, size_words);
    }
    if (shaders_) {
      shaders_->Load(type, words_big_endian, size_words);
    }
  }

  void Draw(bool resolve) override {
    if (!renderer_) {
      return;
    }
    if (resolve) {
      renderer_->Resolve(registers_.file());
    } else {
      renderer_->Draw(registers_.file());
    }
  }

  bool Yield(uint32_t wait) override {
    if (wait >= 0x100 && PerfSkip(kPolledWaits)) {
      rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
    } else if (wait >= 0x100) {
      yield_progress_ = SharedGpuProgress().WaitPast(yield_progress_, std::chrono::milliseconds(wait / 0x100));
    } else {
      rex::thread::MaybeYield();
    }
    return active_.load(std::memory_order_acquire);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const uint64_t frequency = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t interval = std::max(uint64_t(1), uint64_t(double(frequency) / hz));
    uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
    while (active_.load(std::memory_order_acquire)) {
      const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
      while (now - last >= interval) {
        executor_.IncrementCounter();
        Interrupt(0, 2);
        last += interval;
      }
      if (PerfSkip(kPolledWaits)) {
        rex::thread::Sleep(std::chrono::milliseconds(1));
      } else if (const uint64_t elapsed = rex::chrono::Clock::QueryGuestTickCount() - last; elapsed < interval) {
        rex::thread::Sleep(std::chrono::microseconds((interval - elapsed) * 1000000 / frequency));
      }
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t partial_write_pointer = kNoPartialPacket;
    uint32_t generation = ring_.generation();
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_waiting_.store(true, std::memory_order_seq_cst);
        ring_cv_.wait_for(lock, kRingWaitTimeout, [&] {
          return !active_.load(std::memory_order_acquire) || ring_.generation() != generation ||
                 ring_.HasNewWork(read, partial_write_pointer);
        });
        ring_waiting_.store(false, std::memory_order_relaxed);
      }
      ReportIfDue();
      if (ring_.generation() != generation) {
        generation = ring_.generation();
        read = 0;
        partial_write_pointer = kNoPartialPacket;
      }
      const uint32_t size = ring_.size_words();
      if (!size || !ring_.HasNewWork(read, partial_write_pointer)) {
        continue;
      }
      const uint32_t write_pointer = ring_.write_pointer();
      RingReader reader(Words(ring_.base()), size, read, write_pointer & (size - 1));
      const Pm4Executor::Result result =
          executor_.Execute(reader, publish_step_.load(std::memory_order_acquire));
      if (result == Pm4Executor::Result::kAborted) {
        break;
      }
      partial_write_pointer = result == Pm4Executor::Result::kPartial ? write_pointer : kNoPartialPacket;
      read = reader.position();
      PublishReadPointer(read);
    }
    return 0;
  }

  static uint64_t ThreadCpuUs() {
#if REX_PLATFORM_SWITCH
    return 0;
#else
    timespec cpu{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    return uint64_t(cpu.tv_sec) * 1000000 + uint64_t(cpu.tv_nsec) / 1000;
#endif
  }

  void ReportIfDue() {
    const auto now = Clock::now();
    if (now - report_start_ < kReportInterval) {
      return;
    }
    report_start_ = now;
    const Pm4Stats& s = executor_.stats();
    const NativeShaders::Stats t = shaders_ ? shaders_->stats() : NativeShaders::Stats{};
    REXLOG_INFO(
        "[native] 10 s: {} packets, {} draws, {} resolves, {} swaps, {} interrupts, {} shader loads, {} waits ({} slow, "
        "{} ms, last {} {:X} ref {:X} mask {:X} value {:X} func {}), {} "
        "overflows, {} coher events {} MB, {} occlusion events ({} results), {} progress wakes, {} unique shaders, {} shader translations, {} specialized, {} failed, "
        "{} ms translating (max {} ms), ring thread cpu {} ms",
        s.packets - last_.packets, s.draws - last_.draws, s.resolves - last_.resolves, s.swaps - last_.swaps,
        s.interrupts - last_.interrupts, s.shader_loads - last_.shader_loads, s.waits - last_.waits,
        s.slow_waits - last_.slow_waits, (s.slow_wait_us - last_.slow_wait_us) / 1000,
        s.slow_wait_info & 0x10 ? "mem" : "reg", s.slow_wait_poll, s.slow_wait_reference, s.slow_wait_mask,
        s.slow_wait_value, s.slow_wait_info & 7,
        s.overflows - last_.overflows, s.coher_events - last_.coher_events,
        (s.coher_bytes - last_.coher_bytes) >> 20, s.zpd_events - last_.zpd_events, s.zpd_results - last_.zpd_results,
        SharedGpuProgress().notifies() - reported_notifies_,
        shader_dump_ ? shader_dump_->unique() : 0, t.translations, t.specialized,
        t.failures, t.translate_us / 1000, t.max_translate_us / 1000, (ThreadCpuUs() - reported_cpu_us_) / 1000);
    reported_cpu_us_ = ThreadCpuUs();
    last_ = s;
    reported_notifies_ = SharedGpuProgress().notifies();
    REXLOG_INFO("[native] {}", NierNpcLodReport(REXCVAR_GET(nier_npc_update_scale)));
    REXLOG_INFO("[native] {}", NierFramePacingReport());
    if (renderer_) {
      REXLOG_INFO("{}", renderer_->Report());
    }
  }

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  GpuRegisters registers_;
  Pm4Executor executor_{registers_, *this};
  std::unique_ptr<ShaderDump> shader_dump_;
  std::unique_ptr<NativeShaders> shaders_;
  std::unique_ptr<NativeRenderer> renderer_;
  RingState ring_;
  std::atomic<bool> active_{false};
  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> read_writeback_{0};
  uint64_t reported_notifies_ = 0;
  uint64_t reported_cpu_us_ = 0;
  std::atomic<uint32_t> publish_step_{kDefaultPublishStep};
  std::atomic<bool> ring_waiting_{false};
  uint64_t yield_progress_ = 0;
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  rex::system::object_ref<rex::system::XHostThread> record_thread_;
  Clock::time_point report_start_;
  Pm4Stats last_;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace nier::native
