#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "pipeline_report.h"
#include "pipeline_state.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

class Pipelines {
 public:
  enum class Status { kReady, kPending, kFailed };

  struct Stats {
    uint64_t compiled = 0;
    uint64_t compile_us = 0;
    uint64_t max_compile_us = 0;
    uint64_t saved_bytes = 0;
  };

  Pipelines(const rex::ui::vulkan::VulkanDevice* device, PipelineReport& report);
  ~Pipelines();
  Pipelines(const Pipelines&) = delete;
  Pipelines& operator=(const Pipelines&) = delete;

  VkDescriptorSetLayout SharedMemoryLayout() const { return shared_memory_layout_; }
  VkDescriptorSetLayout ConstantsLayout() const { return constants_layout_; }
  uint32_t shared_memory_binding_count() const { return shared_memory_binding_count_; }
  size_t pipeline_count() const { return pipeline_count_; }
  VkPipelineCache cache() const { return cache_; }
  Stats TakeStats();

  VkDescriptorSetLayout TextureSetLayout(bool pixel, uint32_t textures, uint32_t samplers);
  VkPipelineLayout Layout(uint32_t vs_textures, uint32_t vs_samplers, uint32_t ps_textures, uint32_t ps_samplers);
  VkPipeline Get(const PipelineState& state, VkPipelineLayout layout, uint64_t vs_hash, uint64_t ps_hash,
                 Status& status);

 private:
  struct Slot {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<bool> done{false};
  };
  struct Entry {
    PipelineState state;
    VkPipelineLayout layout;
    std::unique_ptr<Slot> slot;
  };
  struct Job {
    PipelineState state;
    VkPipelineLayout layout;
    uint64_t vs_hash;
    uint64_t ps_hash;
    Slot* slot;
  };

  VkPipeline Create(const PipelineState& state, VkPipelineLayout layout) const;
  void Compile(const Job& job);
  void Work();
  void LoadCache();
  void SaveCache();

  const rex::ui::vulkan::VulkanDevice* device_;
  PipelineReport& report_;
  uint32_t shared_memory_binding_count_;
  VkDescriptorSetLayout shared_memory_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_layouts_;
  std::unordered_map<uint64_t, VkPipelineLayout> layouts_;
  std::unordered_map<uint64_t, std::vector<Entry>> pipelines_;
  size_t pipeline_count_ = 0;
  VkPipelineCreateFlags create_flags_;
  std::string cache_path_;
  bool async_;
  std::chrono::milliseconds wait_;

  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable work_done_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  uint64_t unsaved_ = 0;
  std::chrono::steady_clock::time_point last_save_;
  Stats stats_;
  std::thread worker_;
};

}  // namespace nier::native
