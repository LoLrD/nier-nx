#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include <rex/ui/vulkan/device.h>

namespace nier::native {

class VulkanRecorder {
 public:
  using Functions = rex::ui::vulkan::VulkanDevice::Functions;

  static constexpr size_t kQueueBytes = 4u << 20;
  static constexpr uint32_t kWakeBatch = 32;

  explicit VulkanRecorder(const Functions& real);
  ~VulkanRecorder();
  VulkanRecorder(const VulkanRecorder&) = delete;
  VulkanRecorder& operator=(const VulkanRecorder&) = delete;

  const Functions& proxy() const { return proxy_; }
  const Functions& real() const { return real_; }

  bool running() const { return running_.load(std::memory_order_acquire); }
  void Run();
  void Stop();
  void Drain();

  uint64_t commands() const { return commands_.load(std::memory_order_relaxed); }
  uint64_t drains() const { return drains_; }
  uint64_t drain_waits() const { return drain_waits_; }
  uint64_t drain_wait_us() const { return drain_wait_us_; }
  uint64_t full_waits() const { return full_waits_; }
  uint64_t wakeups() const { return wakeups_; }

  template <typename Args, void (*Replay)(const Args&, const Functions&), typename Fill>
  void Emit(size_t extra, Fill&& fill) {
    const size_t bytes = Align(sizeof(Header)) + Align(sizeof(Args)) + Align(extra);
    uint8_t* block = Reserve(bytes);
    Header* header = reinterpret_cast<Header*>(block);
    header->run = &Trampoline<Args, Replay>;
    header->bytes = uint32_t(bytes);
    uint8_t* cursor = block + Align(sizeof(Header)) + Align(sizeof(Args));
    fill(*reinterpret_cast<Args*>(block + Align(sizeof(Header))), cursor);
    Commit(bytes);
  }

  template <typename T>
  static T* Copy(uint8_t*& cursor, const T* source, uint32_t count) {
    if (!source || !count) {
      return nullptr;
    }
    T* copy = reinterpret_cast<T*>(cursor);
    std::memcpy(copy, source, sizeof(T) * count);
    cursor += Align(sizeof(T) * count);
    return copy;
  }

  template <typename T>
  static size_t Bytes(const T* source, uint32_t count) {
    return source ? Align(sizeof(T) * count) : 0;
  }

  static size_t Align(size_t bytes) { return (bytes + 15) & ~size_t(15); }

 private:
  struct alignas(16) Header {
    void (*run)(const uint8_t* args, const Functions& functions);
    uint32_t bytes;
  };

  template <typename Args, void (*Replay)(const Args&, const Functions&)>
  static void Trampoline(const uint8_t* args, const Functions& functions) {
    Replay(*reinterpret_cast<const Args*>(args), functions);
  }

  uint8_t* Reserve(size_t bytes);
  void Commit(size_t bytes);
  void WaitForSpace(size_t bytes);
  void Wake();

  Functions real_;
  Functions proxy_;
  std::unique_ptr<uint8_t[]> queue_;
  uint64_t head_local_ = 0;
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> tail_{0};
  std::atomic<bool> consumer_sleeping_{false};
  std::atomic<bool> producer_waiting_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> commands_{0};
  std::mutex mutex_;
  std::condition_variable consumer_cv_;
  std::condition_variable producer_cv_;
  uint64_t drains_ = 0;
  uint64_t drain_waits_ = 0;
  uint64_t drain_wait_us_ = 0;
  uint64_t full_waits_ = 0;
  uint64_t wakeups_ = 0;
  uint32_t unnotified_ = 0;
};

const VulkanRecorder::Functions& Vk(const rex::ui::vulkan::VulkanDevice* device);
VulkanRecorder* ActiveRecorder();
void SetActiveRecorder(VulkanRecorder* recorder);

}  // namespace nier::native
