#pragma once

#include <span>
#include <vector>

#include "cpk_toc.h"
#include "language_patches.h"

namespace nier {

std::vector<TranslatedFile> LoadTranslatedFiles(std::span<const CpkFile> toc);

}  // namespace nier
