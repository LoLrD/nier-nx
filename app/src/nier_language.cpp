#include "nier_language.h"

#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(nier_language_dir, "", "NieR",
                      "Folder with translated game files laid out like the archive and listed in files.txt; the "
                      "texts replace the English ones. Empty keeps the original files");

namespace nier {

std::vector<TranslatedFile> LoadTranslatedFiles(std::span<const CpkFile> toc) {
  const std::string dir = REXCVAR_GET(nier_language_dir);
  std::vector<TranslatedFile> files;
  if (dir.empty()) {
    return files;
  }
  std::ifstream manifest(dir + "/files.txt");
  std::unordered_set<std::string> listed;
  for (std::string line; std::getline(manifest, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    listed.insert(line);
  }
  for (const CpkFile& file : toc) {
    if (!listed.contains(file.path)) {
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
