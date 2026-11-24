#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "vk_recorder.h"

using nier::native::VulkanRecorder;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static std::vector<uint32_t> draws;
static std::vector<uint32_t> offsets_seen;
static bool depth_is_stencil = false;
static VkImageView depth_view = VK_NULL_HANDLE;
static std::thread::id replay_thread;

static VKAPI_ATTR void VKAPI_CALL FakeDraw(VkCommandBuffer, uint32_t vertices, uint32_t, uint32_t, uint32_t) {
  if (vertices == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  draws.push_back(vertices);
  replay_thread = std::this_thread::get_id();
}

static VKAPI_ATTR void VKAPI_CALL FakeBindSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                               uint32_t, const VkDescriptorSet*, uint32_t count,
                                               const uint32_t* offsets) {
  offsets_seen.assign(offsets, offsets + count);
}

static VKAPI_ATTR void VKAPI_CALL FakeBeginRendering(VkCommandBuffer, const VkRenderingInfo* info) {
  depth_is_stencil = info->pDepthAttachment == info->pStencilAttachment;
  depth_view = info->pDepthAttachment->imageView;
}

int main() {
  VulkanRecorder::Functions real{};
  real.vkCmdDraw = FakeDraw;
  real.vkCmdBindDescriptorSets = FakeBindSets;
  real.vkCmdBeginRendering = FakeBeginRendering;
  VulkanRecorder recorder(real);
  std::thread worker([&] { recorder.Run(); });
  const auto& vk = recorder.proxy();
  constexpr uint32_t kDraws = 300000;
  for (uint32_t i = 0; i < kDraws; ++i) {
    vk.vkCmdDraw(VK_NULL_HANDLE, i, 1, 0, 0);
  }
  uint32_t offsets[3] = {16, 32, 48};
  vk.vkCmdBindDescriptorSets(VK_NULL_HANDLE, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, 1, 0, nullptr, 3,
                             offsets);
  offsets[0] = 99;
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = reinterpret_cast<VkImageView>(uintptr_t(0x1234));
  VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.pDepthAttachment = &depth;
  info.pStencilAttachment = &depth;
  vk.vkCmdBeginRendering(VK_NULL_HANDLE, &info);
  depth.imageView = VK_NULL_HANDLE;
  recorder.Drain();
  bool ordered = draws.size() == kDraws;
  for (uint32_t i = 0; ordered && i < kDraws; ++i) {
    ordered = draws[i] == i;
  }
  Check(ordered, "every_command_replayed_in_order");
  Check(recorder.full_waits() > 0, "queue_wrapped_and_waited_for_space");
  Check(offsets_seen == std::vector<uint32_t>{16, 32, 48}, "arrays_copied_at_record_time");
  Check(depth_is_stencil && depth_view == reinterpret_cast<VkImageView>(uintptr_t(0x1234)),
        "rendering_info_copied_with_shared_depth_stencil");
  Check(replay_thread != std::this_thread::get_id(), "commands_replayed_on_worker");
  vk.vkCmdDraw(VK_NULL_HANDLE, 7, 1, 0, 0);
  recorder.Drain();
  Check(draws.back() == 7, "drain_waits_for_late_command");
  const uint64_t wakeups_before = recorder.wakeups();
  const uint64_t commands_before = recorder.commands();
  for (uint32_t i = 0; i < 4000; ++i) {
    vk.vkCmdDraw(VK_NULL_HANDLE, 1, 1, 0, 0);
    if (i % 3 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  }
  recorder.Drain();
  const uint64_t woken = recorder.wakeups() - wakeups_before;
  Check(woken <= (recorder.commands() - commands_before) / VulkanRecorder::kWakeBatch + 1,
        "worker_woken_at_most_once_per_batch");
  recorder.Stop();
  worker.join();
  return failures ? 1 : 0;
}
