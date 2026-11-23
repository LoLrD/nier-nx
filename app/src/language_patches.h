#pragma once

#include <span>
#include <string>
#include <vector>

#include "cpk_toc.h"
#include "read_patches.h"

namespace nier {

struct TranslatedFile {
  std::string path;
  std::vector<uint8_t> bytes;
};

struct LanguagePlan {
  std::vector<ReadPatch> patches;
  std::vector<std::string> left_out;
};

LanguagePlan PlanLanguage(std::span<const CpkFile> toc, std::span<const TranslatedFile> files);

}  // namespace nier
