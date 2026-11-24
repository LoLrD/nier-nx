#include "vk_recorder.h"

#include <chrono>
#include <cstring>

namespace nier::native {

namespace {

using Functions = VulkanRecorder::Functions;

VulkanRecorder* recorder = nullptr;
VulkanRecorder* active = nullptr;

template <typename T>
size_t Bytes(const T* source, uint32_t count) {
  return VulkanRecorder::Bytes(source, count);
}

template <typename T>
T* Copy(uint8_t*& cursor, const T* source, uint32_t count) {
  return VulkanRecorder::Copy(cursor, source, count);
}

struct BindPipeline {
  VkCommandBuffer cmd;
  VkPipelineBindPoint point;
  VkPipeline pipeline;
};
void Replay(const BindPipeline& a, const Functions& f) { f.vkCmdBindPipeline(a.cmd, a.point, a.pipeline); }
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
  recorder->Emit<BindPipeline, Replay>(0, [&](BindPipeline& a, uint8_t*&) { a = {cmd, point, pipeline}; });
}

struct BindDescriptorSets {
  VkCommandBuffer cmd;
  VkPipelineBindPoint point;
  VkPipelineLayout layout;
  uint32_t first;
  uint32_t count;
  const VkDescriptorSet* sets;
  uint32_t offset_count;
  const uint32_t* offsets;
};
void Replay(const BindDescriptorSets& a, const Functions& f) {
  f.vkCmdBindDescriptorSets(a.cmd, a.point, a.layout, a.first, a.count, a.sets, a.offset_count, a.offsets);
}
VKAPI_ATTR void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                                 VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                 const VkDescriptorSet* sets, uint32_t offset_count,
                                                 const uint32_t* offsets) {
  recorder->Emit<BindDescriptorSets, Replay>(
      Bytes(sets, count) + Bytes(offsets, offset_count), [&](BindDescriptorSets& a, uint8_t*& extra) {
        a = {cmd, point, layout, first, count, Copy(extra, sets, count), offset_count,
             Copy(extra, offsets, offset_count)};
      });
}

struct SetViewport {
  VkCommandBuffer cmd;
  uint32_t first;
  uint32_t count;
  const VkViewport* viewports;
};
void Replay(const SetViewport& a, const Functions& f) { f.vkCmdSetViewport(a.cmd, a.first, a.count, a.viewports); }
VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                          const VkViewport* viewports) {
  recorder->Emit<SetViewport, Replay>(Bytes(viewports, count), [&](SetViewport& a, uint8_t*& extra) {
    a = {cmd, first, count, Copy(extra, viewports, count)};
  });
}

struct SetScissor {
  VkCommandBuffer cmd;
  uint32_t first;
  uint32_t count;
  const VkRect2D* rects;
};
void Replay(const SetScissor& a, const Functions& f) { f.vkCmdSetScissor(a.cmd, a.first, a.count, a.rects); }
VKAPI_ATTR void VKAPI_CALL CmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D* rects) {
  recorder->Emit<SetScissor, Replay>(Bytes(rects, count), [&](SetScissor& a, uint8_t*& extra) {
    a = {cmd, first, count, Copy(extra, rects, count)};
  });
}

struct SetDepthBias {
  VkCommandBuffer cmd;
  float constant;
  float clamp;
  float slope;
};
void Replay(const SetDepthBias& a, const Functions& f) { f.vkCmdSetDepthBias(a.cmd, a.constant, a.clamp, a.slope); }
VKAPI_ATTR void VKAPI_CALL CmdSetDepthBias(VkCommandBuffer cmd, float constant, float clamp, float slope) {
  recorder->Emit<SetDepthBias, Replay>(0, [&](SetDepthBias& a, uint8_t*&) { a = {cmd, constant, clamp, slope}; });
}

struct SetBlendConstants {
  VkCommandBuffer cmd;
  float blend[4];
};
void Replay(const SetBlendConstants& a, const Functions& f) { f.vkCmdSetBlendConstants(a.cmd, a.blend); }
VKAPI_ATTR void VKAPI_CALL CmdSetBlendConstants(VkCommandBuffer cmd, const float blend[4]) {
  recorder->Emit<SetBlendConstants, Replay>(0, [&](SetBlendConstants& a, uint8_t*&) {
    a.cmd = cmd;
    std::memcpy(a.blend, blend, sizeof(a.blend));
  });
}

struct SetStencil {
  VkCommandBuffer cmd;
  VkStencilFaceFlags faces;
  uint32_t value;
};
void ReplayCompareMask(const SetStencil& a, const Functions& f) {
  f.vkCmdSetStencilCompareMask(a.cmd, a.faces, a.value);
}
void ReplayWriteMask(const SetStencil& a, const Functions& f) { f.vkCmdSetStencilWriteMask(a.cmd, a.faces, a.value); }
void ReplayReference(const SetStencil& a, const Functions& f) { f.vkCmdSetStencilReference(a.cmd, a.faces, a.value); }
VKAPI_ATTR void VKAPI_CALL CmdSetStencilCompareMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t value) {
  recorder->Emit<SetStencil, ReplayCompareMask>(0, [&](SetStencil& a, uint8_t*&) { a = {cmd, faces, value}; });
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilWriteMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t value) {
  recorder->Emit<SetStencil, ReplayWriteMask>(0, [&](SetStencil& a, uint8_t*&) { a = {cmd, faces, value}; });
}
VKAPI_ATTR void VKAPI_CALL CmdSetStencilReference(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t value) {
  recorder->Emit<SetStencil, ReplayReference>(0, [&](SetStencil& a, uint8_t*&) { a = {cmd, faces, value}; });
}

struct BindIndexBuffer {
  VkCommandBuffer cmd;
  VkBuffer buffer;
  VkDeviceSize offset;
  VkIndexType type;
};
void Replay(const BindIndexBuffer& a, const Functions& f) {
  f.vkCmdBindIndexBuffer(a.cmd, a.buffer, a.offset, a.type);
}
VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                              VkIndexType type) {
  recorder->Emit<BindIndexBuffer, Replay>(0, [&](BindIndexBuffer& a, uint8_t*&) { a = {cmd, buffer, offset, type}; });
}

struct Draw {
  VkCommandBuffer cmd;
  uint32_t vertices;
  uint32_t instances;
  uint32_t first_vertex;
  uint32_t first_instance;
};
void Replay(const Draw& a, const Functions& f) {
  f.vkCmdDraw(a.cmd, a.vertices, a.instances, a.first_vertex, a.first_instance);
}
VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                   uint32_t first_instance) {
  recorder->Emit<Draw, Replay>(0, [&](Draw& a, uint8_t*&) { a = {cmd, vertices, instances, first_vertex, first_instance}; });
}

struct DrawIndexed {
  VkCommandBuffer cmd;
  uint32_t indices;
  uint32_t instances;
  uint32_t first_index;
  int32_t vertex_offset;
  uint32_t first_instance;
};
void Replay(const DrawIndexed& a, const Functions& f) {
  f.vkCmdDrawIndexed(a.cmd, a.indices, a.instances, a.first_index, a.vertex_offset, a.first_instance);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances,
                                          uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  recorder->Emit<DrawIndexed, Replay>(0, [&](DrawIndexed& a, uint8_t*&) {
    a = {cmd, indices, instances, first_index, vertex_offset, first_instance};
  });
}

struct Dispatch {
  VkCommandBuffer cmd;
  uint32_t x;
  uint32_t y;
  uint32_t z;
};
void Replay(const Dispatch& a, const Functions& f) { f.vkCmdDispatch(a.cmd, a.x, a.y, a.z); }
VKAPI_ATTR void VKAPI_CALL CmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
  recorder->Emit<Dispatch, Replay>(0, [&](Dispatch& a, uint8_t*&) { a = {cmd, x, y, z}; });
}

struct PushConstants {
  VkCommandBuffer cmd;
  VkPipelineLayout layout;
  VkShaderStageFlags stages;
  uint32_t offset;
  uint32_t size;
  const uint8_t* values;
};
void Replay(const PushConstants& a, const Functions& f) {
  f.vkCmdPushConstants(a.cmd, a.layout, a.stages, a.offset, a.size, a.values);
}
VKAPI_ATTR void VKAPI_CALL CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                                            uint32_t offset, uint32_t size, const void* values) {
  const auto* bytes = static_cast<const uint8_t*>(values);
  recorder->Emit<PushConstants, Replay>(Bytes(bytes, size), [&](PushConstants& a, uint8_t*& extra) {
    a = {cmd, layout, stages, offset, size, Copy(extra, bytes, size)};
  });
}

struct PipelineBarrier {
  VkCommandBuffer cmd;
  VkPipelineStageFlags source;
  VkPipelineStageFlags destination;
  VkDependencyFlags dependency;
  uint32_t memory_count;
  const VkMemoryBarrier* memory;
  uint32_t buffer_count;
  const VkBufferMemoryBarrier* buffers;
  uint32_t image_count;
  const VkImageMemoryBarrier* images;
};
void Replay(const PipelineBarrier& a, const Functions& f) {
  f.vkCmdPipelineBarrier(a.cmd, a.source, a.destination, a.dependency, a.memory_count, a.memory, a.buffer_count,
                         a.buffers, a.image_count, a.images);
}
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags source,
                                              VkPipelineStageFlags destination, VkDependencyFlags dependency,
                                              uint32_t memory_count, const VkMemoryBarrier* memory,
                                              uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                                              uint32_t image_count, const VkImageMemoryBarrier* images) {
  recorder->Emit<PipelineBarrier, Replay>(
      Bytes(memory, memory_count) + Bytes(buffers, buffer_count) + Bytes(images, image_count),
      [&](PipelineBarrier& a, uint8_t*& extra) {
        a = {cmd,
             source,
             destination,
             dependency,
             memory_count,
             Copy(extra, memory, memory_count),
             buffer_count,
             Copy(extra, buffers, buffer_count),
             image_count,
             Copy(extra, images, image_count)};
      });
}

struct ClearColorImage {
  VkCommandBuffer cmd;
  VkImage image;
  VkImageLayout layout;
  VkClearColorValue color;
  uint32_t count;
  const VkImageSubresourceRange* ranges;
};
void Replay(const ClearColorImage& a, const Functions& f) {
  f.vkCmdClearColorImage(a.cmd, a.image, a.layout, &a.color, a.count, a.ranges);
}
VKAPI_ATTR void VKAPI_CALL CmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                                              const VkClearColorValue* color, uint32_t count,
                                              const VkImageSubresourceRange* ranges) {
  recorder->Emit<ClearColorImage, Replay>(Bytes(ranges, count), [&](ClearColorImage& a, uint8_t*& extra) {
    a = {cmd, image, layout, *color, count, Copy(extra, ranges, count)};
  });
}

struct ClearDepthStencilImage {
  VkCommandBuffer cmd;
  VkImage image;
  VkImageLayout layout;
  VkClearDepthStencilValue value;
  uint32_t count;
  const VkImageSubresourceRange* ranges;
};
void Replay(const ClearDepthStencilImage& a, const Functions& f) {
  f.vkCmdClearDepthStencilImage(a.cmd, a.image, a.layout, &a.value, a.count, a.ranges);
}
VKAPI_ATTR void VKAPI_CALL CmdClearDepthStencilImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                                                     const VkClearDepthStencilValue* value, uint32_t count,
                                                     const VkImageSubresourceRange* ranges) {
  recorder->Emit<ClearDepthStencilImage, Replay>(Bytes(ranges, count),
                                                 [&](ClearDepthStencilImage& a, uint8_t*& extra) {
                                                   a = {cmd, image, layout, *value, count,
                                                        Copy(extra, ranges, count)};
                                                 });
}

struct ClearAttachments {
  VkCommandBuffer cmd;
  uint32_t attachment_count;
  const VkClearAttachment* attachments;
  uint32_t rect_count;
  const VkClearRect* rects;
};
void Replay(const ClearAttachments& a, const Functions& f) {
  f.vkCmdClearAttachments(a.cmd, a.attachment_count, a.attachments, a.rect_count, a.rects);
}
VKAPI_ATTR void VKAPI_CALL CmdClearAttachments(VkCommandBuffer cmd, uint32_t attachment_count,
                                               const VkClearAttachment* attachments, uint32_t rect_count,
                                               const VkClearRect* rects) {
  recorder->Emit<ClearAttachments, Replay>(
      Bytes(attachments, attachment_count) + Bytes(rects, rect_count), [&](ClearAttachments& a, uint8_t*& extra) {
        a = {cmd, attachment_count, Copy(extra, attachments, attachment_count), rect_count,
             Copy(extra, rects, rect_count)};
      });
}

struct CopyImage {
  VkCommandBuffer cmd;
  VkImage source;
  VkImageLayout source_layout;
  VkImage destination;
  VkImageLayout destination_layout;
  uint32_t count;
  const VkImageCopy* regions;
};
void Replay(const CopyImage& a, const Functions& f) {
  f.vkCmdCopyImage(a.cmd, a.source, a.source_layout, a.destination, a.destination_layout, a.count, a.regions);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImage(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout,
                                        VkImage destination, VkImageLayout destination_layout, uint32_t count,
                                        const VkImageCopy* regions) {
  recorder->Emit<CopyImage, Replay>(Bytes(regions, count), [&](CopyImage& a, uint8_t*& extra) {
    a = {cmd, source, source_layout, destination, destination_layout, count, Copy(extra, regions, count)};
  });
}

struct CopyImageToBuffer {
  VkCommandBuffer cmd;
  VkImage source;
  VkImageLayout layout;
  VkBuffer destination;
  uint32_t count;
  const VkBufferImageCopy* regions;
};
void Replay(const CopyImageToBuffer& a, const Functions& f) {
  f.vkCmdCopyImageToBuffer(a.cmd, a.source, a.layout, a.destination, a.count, a.regions);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage source, VkImageLayout layout,
                                                VkBuffer destination, uint32_t count,
                                                const VkBufferImageCopy* regions) {
  recorder->Emit<CopyImageToBuffer, Replay>(Bytes(regions, count), [&](CopyImageToBuffer& a, uint8_t*& extra) {
    a = {cmd, source, layout, destination, count, Copy(extra, regions, count)};
  });
}

struct CopyBufferToImage {
  VkCommandBuffer cmd;
  VkBuffer source;
  VkImage destination;
  VkImageLayout layout;
  uint32_t count;
  const VkBufferImageCopy* regions;
};
void Replay(const CopyBufferToImage& a, const Functions& f) {
  f.vkCmdCopyBufferToImage(a.cmd, a.source, a.destination, a.layout, a.count, a.regions);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer source, VkImage destination,
                                                VkImageLayout layout, uint32_t count,
                                                const VkBufferImageCopy* regions) {
  recorder->Emit<CopyBufferToImage, Replay>(Bytes(regions, count), [&](CopyBufferToImage& a, uint8_t*& extra) {
    a = {cmd, source, destination, layout, count, Copy(extra, regions, count)};
  });
}

struct CopyBuffer {
  VkCommandBuffer cmd;
  VkBuffer source;
  VkBuffer destination;
  uint32_t count;
  const VkBufferCopy* regions;
};
void Replay(const CopyBuffer& a, const Functions& f) {
  f.vkCmdCopyBuffer(a.cmd, a.source, a.destination, a.count, a.regions);
}
VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cmd, VkBuffer source, VkBuffer destination, uint32_t count,
                                         const VkBufferCopy* regions) {
  recorder->Emit<CopyBuffer, Replay>(Bytes(regions, count), [&](CopyBuffer& a, uint8_t*& extra) {
    a = {cmd, source, destination, count, Copy(extra, regions, count)};
  });
}

struct BlitImage {
  VkCommandBuffer cmd;
  VkImage source;
  VkImageLayout source_layout;
  VkImage destination;
  VkImageLayout destination_layout;
  uint32_t count;
  const VkImageBlit* regions;
  VkFilter filter;
};
void Replay(const BlitImage& a, const Functions& f) {
  f.vkCmdBlitImage(a.cmd, a.source, a.source_layout, a.destination, a.destination_layout, a.count, a.regions,
                   a.filter);
}
VKAPI_ATTR void VKAPI_CALL CmdBlitImage(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout,
                                        VkImage destination, VkImageLayout destination_layout, uint32_t count,
                                        const VkImageBlit* regions, VkFilter filter) {
  recorder->Emit<BlitImage, Replay>(Bytes(regions, count), [&](BlitImage& a, uint8_t*& extra) {
    a = {cmd, source, source_layout, destination, destination_layout, count, Copy(extra, regions, count), filter};
  });
}

struct BeginRendering {
  VkCommandBuffer cmd;
  VkRenderingInfo info;
};
void Replay(const BeginRendering& a, const Functions& f) { f.vkCmdBeginRendering(a.cmd, &a.info); }
VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
  const bool same = info->pDepthAttachment && info->pDepthAttachment == info->pStencilAttachment;
  recorder->Emit<BeginRendering, Replay>(
      Bytes(info->pColorAttachments, info->colorAttachmentCount) + Bytes(info->pDepthAttachment, 1) +
          Bytes(info->pStencilAttachment, 1),
      [&](BeginRendering& a, uint8_t*& extra) {
        a.cmd = cmd;
        a.info = *info;
        a.info.pColorAttachments = Copy(extra, info->pColorAttachments, info->colorAttachmentCount);
        a.info.pDepthAttachment = Copy(extra, info->pDepthAttachment, 1);
        a.info.pStencilAttachment = same ? a.info.pDepthAttachment : Copy(extra, info->pStencilAttachment, 1);
      });
}

struct EndRendering {
  VkCommandBuffer cmd;
};
void Replay(const EndRendering& a, const Functions& f) { f.vkCmdEndRendering(a.cmd); }
VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer cmd) {
  recorder->Emit<EndRendering, Replay>(0, [&](EndRendering& a, uint8_t*&) { a = {cmd}; });
}

struct WriteTimestamp {
  VkCommandBuffer cmd;
  VkPipelineStageFlagBits stage;
  VkQueryPool pool;
  uint32_t query;
};
void Replay(const WriteTimestamp& a, const Functions& f) { f.vkCmdWriteTimestamp(a.cmd, a.stage, a.pool, a.query); }
VKAPI_ATTR void VKAPI_CALL CmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool,
                                             uint32_t query) {
  recorder->Emit<WriteTimestamp, Replay>(0, [&](WriteTimestamp& a, uint8_t*&) { a = {cmd, stage, pool, query}; });
}

struct ResetQueryPool {
  VkCommandBuffer cmd;
  VkQueryPool pool;
  uint32_t first;
  uint32_t count;
};
void Replay(const ResetQueryPool& a, const Functions& f) { f.vkCmdResetQueryPool(a.cmd, a.pool, a.first, a.count); }
VKAPI_ATTR void VKAPI_CALL CmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first, uint32_t count) {
  recorder->Emit<ResetQueryPool, Replay>(0, [&](ResetQueryPool& a, uint8_t*&) { a = {cmd, pool, first, count}; });
}

}  // namespace

VulkanRecorder::VulkanRecorder(const Functions& real)
    : real_(real), proxy_(real), queue_(std::make_unique<uint8_t[]>(kQueueBytes)) {
  recorder = this;
  proxy_.vkCmdBindPipeline = CmdBindPipeline;
  proxy_.vkCmdBindDescriptorSets = CmdBindDescriptorSets;
  proxy_.vkCmdSetViewport = CmdSetViewport;
  proxy_.vkCmdSetScissor = CmdSetScissor;
  proxy_.vkCmdSetDepthBias = CmdSetDepthBias;
  proxy_.vkCmdSetBlendConstants = CmdSetBlendConstants;
  proxy_.vkCmdSetStencilCompareMask = CmdSetStencilCompareMask;
  proxy_.vkCmdSetStencilWriteMask = CmdSetStencilWriteMask;
  proxy_.vkCmdSetStencilReference = CmdSetStencilReference;
  proxy_.vkCmdBindIndexBuffer = CmdBindIndexBuffer;
  proxy_.vkCmdDraw = CmdDraw;
  proxy_.vkCmdDrawIndexed = CmdDrawIndexed;
  proxy_.vkCmdDispatch = CmdDispatch;
  proxy_.vkCmdPushConstants = CmdPushConstants;
  proxy_.vkCmdPipelineBarrier = CmdPipelineBarrier;
  proxy_.vkCmdClearColorImage = CmdClearColorImage;
  proxy_.vkCmdClearDepthStencilImage = CmdClearDepthStencilImage;
  proxy_.vkCmdClearAttachments = CmdClearAttachments;
  proxy_.vkCmdCopyImage = CmdCopyImage;
  proxy_.vkCmdCopyImageToBuffer = CmdCopyImageToBuffer;
  proxy_.vkCmdCopyBufferToImage = CmdCopyBufferToImage;
  proxy_.vkCmdCopyBuffer = CmdCopyBuffer;
  proxy_.vkCmdBlitImage = CmdBlitImage;
  proxy_.vkCmdBeginRendering = CmdBeginRendering;
  proxy_.vkCmdEndRendering = CmdEndRendering;
  proxy_.vkCmdWriteTimestamp = CmdWriteTimestamp;
  proxy_.vkCmdResetQueryPool = CmdResetQueryPool;
}

VulkanRecorder::~VulkanRecorder() {
  if (recorder == this) {
    recorder = nullptr;
  }
}

uint8_t* VulkanRecorder::Reserve(size_t bytes) {
  size_t position = head_local_ % kQueueBytes;
  const size_t tail_room = kQueueBytes - position;
  WaitForSpace(bytes <= tail_room ? bytes : bytes + tail_room);
  if (bytes > tail_room) {
    Header* wrap = reinterpret_cast<Header*>(queue_.get() + position);
    wrap->run = nullptr;
    wrap->bytes = uint32_t(tail_room);
    head_local_ += tail_room;
    position = 0;
  }
  return queue_.get() + position;
}

void VulkanRecorder::WaitForSpace(size_t bytes) {
  if (head_local_ + bytes - tail_.load(std::memory_order_seq_cst) <= kQueueBytes) {
    return;
  }
  ++full_waits_;
  head_.store(head_local_, std::memory_order_seq_cst);
  std::unique_lock lock(mutex_);
  producer_waiting_.store(true, std::memory_order_seq_cst);
  consumer_cv_.notify_one();
  producer_cv_.wait(lock, [&] { return head_local_ + bytes - tail_.load(std::memory_order_seq_cst) <= kQueueBytes; });
  producer_waiting_.store(false, std::memory_order_seq_cst);
}

void VulkanRecorder::Commit(size_t bytes) {
  head_local_ += bytes;
  head_.store(head_local_, std::memory_order_seq_cst);
  commands_.fetch_add(1, std::memory_order_relaxed);
  if (consumer_sleeping_.load(std::memory_order_seq_cst)) {
    std::lock_guard lock(mutex_);
    consumer_cv_.notify_one();
  }
}

void VulkanRecorder::Drain() {
  ++drains_;
  if (tail_.load(std::memory_order_seq_cst) == head_local_) {
    return;
  }
  ++drain_waits_;
  const auto start = std::chrono::steady_clock::now();
  std::unique_lock lock(mutex_);
  producer_waiting_.store(true, std::memory_order_seq_cst);
  consumer_cv_.notify_one();
  producer_cv_.wait(lock, [&] { return tail_.load(std::memory_order_seq_cst) == head_local_; });
  producer_waiting_.store(false, std::memory_order_seq_cst);
  drain_wait_us_ += uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

void VulkanRecorder::Run() {
  running_.store(true, std::memory_order_release);
  uint64_t tail = tail_.load(std::memory_order_seq_cst);
  while (true) {
    uint64_t head = head_.load(std::memory_order_seq_cst);
    if (tail == head) {
      std::unique_lock lock(mutex_);
      consumer_sleeping_.store(true, std::memory_order_seq_cst);
      consumer_cv_.wait(lock, [&] {
        return head_.load(std::memory_order_seq_cst) != tail || stop_.load(std::memory_order_seq_cst);
      });
      consumer_sleeping_.store(false, std::memory_order_seq_cst);
      if (head_.load(std::memory_order_seq_cst) == tail) {
        running_.store(false, std::memory_order_release);
        return;
      }
      continue;
    }
    while (tail != head) {
      const size_t position = tail % kQueueBytes;
      const Header* header = reinterpret_cast<const Header*>(queue_.get() + position);
      if (header->run) {
        header->run(queue_.get() + position + Align(sizeof(Header)), real_);
      }
      tail += header->bytes;
    }
    tail_.store(tail, std::memory_order_seq_cst);
    if (producer_waiting_.load(std::memory_order_seq_cst)) {
      std::lock_guard lock(mutex_);
      producer_cv_.notify_one();
    }
  }
}

const VulkanRecorder::Functions& Vk(const rex::ui::vulkan::VulkanDevice* device) {
  return active ? active->proxy() : device->functions();
}

VulkanRecorder* ActiveRecorder() { return active; }

void SetActiveRecorder(VulkanRecorder* next) {
  if (active && active != next) {
    active->Drain();
  }
  active = next;
}

void VulkanRecorder::Stop() {
  std::lock_guard lock(mutex_);
  stop_.store(true, std::memory_order_seq_cst);
  consumer_cv_.notify_one();
}

}  // namespace nier::native
