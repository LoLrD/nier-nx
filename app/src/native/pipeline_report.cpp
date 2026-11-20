#include "pipeline_report.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

REXCVAR_DEFINE_STRING(nier_shader_assembly_file, "", "NieR",
                      "Native renderer: file that collects the NAK assembly of each new shader (needs "
                      "vulkan_pipeline_statistics)");

namespace nier::native {

namespace {

bool Enabled(const rex::ui::vulkan::VulkanDevice* device) {
  return device->extensions().ext_KHR_pipeline_executable_properties && device->properties().pipelineExecutableInfo;
}

std::string Value(const VkPipelineExecutableStatisticKHR& statistic) {
  switch (statistic.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
      return statistic.value.b32 ? "yes" : "no";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
      return std::to_string(statistic.value.i64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
      return std::to_string(statistic.value.u64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
      return std::to_string(statistic.value.f64);
    default:
      return "?";
  }
}

}  // namespace

VkPipelineCreateFlags PipelineCaptureFlags(const rex::ui::vulkan::VulkanDevice* device) {
  if (!Enabled(device)) {
    return 0;
  }
  VkPipelineCreateFlags flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
  if (!REXCVAR_GET(nier_shader_assembly_file).empty()) {
    flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
  }
  return flags;
}

PipelineReport::PipelineReport(const rex::ui::vulkan::VulkanDevice* device)
    : device_(device), enabled_(Enabled(device)), assembly_path_(REXCVAR_GET(nier_shader_assembly_file)) {}

void PipelineReport::Report(VkPipeline pipeline, const std::string& label) {
  if (!enabled_) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, nullptr, pipeline};
  uint32_t count = 0;
  dfn.vkGetPipelineExecutablePropertiesKHR(device_->device(), &info, &count, nullptr);
  std::vector<VkPipelineExecutablePropertiesKHR> properties(count,
                                                            {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
  dfn.vkGetPipelineExecutablePropertiesKHR(device_->device(), &info, &count, properties.data());
  for (uint32_t i = 0; i < count; ++i) {
    const VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, nullptr, pipeline, i};
    REXLOG_INFO("[native] shader {} {}: {}", label, properties[i].name, Statistics(executable));
    if (!assembly_path_.empty()) {
      AppendAssembly(executable, label + " " + properties[i].name);
    }
  }
}

std::string PipelineReport::Statistics(const VkPipelineExecutableInfoKHR& executable) const {
  const auto& dfn = device_->functions();
  uint32_t count = 0;
  dfn.vkGetPipelineExecutableStatisticsKHR(device_->device(), &executable, &count, nullptr);
  std::vector<VkPipelineExecutableStatisticKHR> statistics(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
  dfn.vkGetPipelineExecutableStatisticsKHR(device_->device(), &executable, &count, statistics.data());
  std::string line;
  for (const VkPipelineExecutableStatisticKHR& statistic : statistics) {
    if (!line.empty()) {
      line += ", ";
    }
    line += statistic.name;
    line += " ";
    line += Value(statistic);
  }
  return line;
}

void PipelineReport::AppendAssembly(const VkPipelineExecutableInfoKHR& executable, const std::string& title) {
  const auto& dfn = device_->functions();
  uint32_t count = 0;
  dfn.vkGetPipelineExecutableInternalRepresentationsKHR(device_->device(), &executable, &count, nullptr);
  std::vector<VkPipelineExecutableInternalRepresentationKHR> representations(
      count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
  dfn.vkGetPipelineExecutableInternalRepresentationsKHR(device_->device(), &executable, &count,
                                                        representations.data());
  std::vector<std::string> texts(count);
  for (uint32_t i = 0; i < count; ++i) {
    texts[i].resize(representations[i].dataSize);
    representations[i].pData = texts[i].data();
  }
  dfn.vkGetPipelineExecutableInternalRepresentationsKHR(device_->device(), &executable, &count,
                                                        representations.data());
  for (uint32_t i = 0; i < count; ++i) {
    if (std::strcmp(representations[i].name, "NAK assembly") != 0) {
      continue;
    }
    const std::string& text = texts[i];
    if (!written_.insert(XXH3_64bits(text.data(), text.size())).second) {
      continue;
    }
    std::ofstream file(assembly_path_, std::ios::app);
    file << "==== " << title << "\n" << text.c_str() << "\n";
  }
}

}  // namespace nier::native
