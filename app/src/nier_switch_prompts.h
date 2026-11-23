#pragma once

#include <span>
#include <string>
#include <vector>

#include "cpk_toc.h"
#include "read_patches.h"

namespace nier {

std::vector<ReadPatch> SwitchPromptPatches(std::span<const CpkFile> toc, const CpkReader& read,
                                           const std::string& archive);

}  // namespace nier
