#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace nier::native {

VkPipelineCreateFlags PipelineCaptureFlags(const rex::ui::vulkan::VulkanDevice* device);

class PipelineReport {
 public:
  explicit PipelineReport(const rex::ui::vulkan::VulkanDevice* device);

  void Report(VkPipeline pipeline, const std::string& label);

 private:
  std::string Statistics(const VkPipelineExecutableInfoKHR& executable) const;
  void AppendAssembly(const VkPipelineExecutableInfoKHR& executable, const std::string& title);

  const rex::ui::vulkan::VulkanDevice* device_;
  bool enabled_;
  std::string assembly_path_;
  std::unordered_set<uint64_t> written_;
};

}  // namespace nier::native
