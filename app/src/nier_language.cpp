#include "nier_language.h"

#include <fstream>
#include <iterator>
#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(nier_language_dir, "", "NieR",
                      "Folder with translated game text files laid out like the archive (TEXT/...); they replace "
                      "the English ones. Empty keeps the original texts");

namespace nier {

std::vector<TranslatedFile> LoadTranslatedFiles(std::span<const CpkFile> toc) {
  const std::string dir = REXCVAR_GET(nier_language_dir);
  std::vector<TranslatedFile> files;
  if (dir.empty()) {
    return files;
  }
  for (const CpkFile& file : toc) {
    if (!file.path.starts_with("TEXT/")) {
      continue;
    }
    std::ifstream in(dir + "/" + file.path, std::ios::binary);
    if (!in) {
      continue;
    }
    files.push_back({file.path, {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}});
  }
  return files;
}

}  // namespace nier
