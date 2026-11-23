#include "language_patches.h"

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace nier {

namespace {

constexpr uint64_t kAlign = 0x800;

struct Space {
  uint64_t begin;
  uint64_t end;
};

uint64_t AlignUp(uint64_t value, uint64_t base) { return base + (value - base + kAlign - 1) / kAlign * kAlign; }

bool OtherLanguageText(const std::string& path) {
  return path.starts_with("TEXT/") && (path.find(".FR.") != std::string::npos ||
                                       path.find(".DE.") != std::string::npos ||
                                       path.find(".JA.") != std::string::npos);
}

std::vector<Space> SpareSpace(std::span<const CpkFile> toc) {
  std::vector<const CpkFile*> sorted;
  for (const CpkFile& file : toc) {
    sorted.push_back(&file);
  }
  std::sort(sorted.begin(), sorted.end(), [](const CpkFile* a, const CpkFile* b) { return a->offset < b->offset; });
  std::vector<Space> spaces;
  bool open = false;
  for (const CpkFile* file : sorted) {
    if (!OtherLanguageText(file->path)) {
      open = false;
      continue;
    }
    if (open) {
      spaces.back().end = std::max(spaces.back().end, file->offset + file->size);
    } else {
      spaces.push_back({file->offset, file->offset + file->size});
      open = true;
    }
  }
  return spaces;
}

ReadPatch CellPatch(const CpkCell& cell, uint64_t value) {
  ReadPatch patch{cell.at, std::vector<uint8_t>(cell.width)};
  for (uint8_t i = 0; i < cell.width; ++i) {
    patch.bytes[i] = uint8_t(value >> (8 * (cell.width - 1 - i)));
  }
  return patch;
}

}  // namespace

LanguagePlan PlanLanguage(std::span<const CpkFile> toc, std::span<const TranslatedFile> files) {
  std::vector<Space> spaces = SpareSpace(toc);
  std::vector<size_t> order(files.size());
  std::iota(order.begin(), order.end(), size_t(0));
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return files[a].bytes.size() > files[b].bytes.size(); });
  LanguagePlan plan;
  for (const size_t index : order) {
    const TranslatedFile& file = files[index];
    const auto entry =
        std::find_if(toc.begin(), toc.end(), [&](const CpkFile& candidate) { return candidate.path == file.path; });
    if (entry == toc.end() || !entry->size_cell.width || !entry->offset_cell.width) {
      plan.left_out.push_back(file.path);
      continue;
    }
    const auto space = std::find_if(spaces.begin(), spaces.end(), [&](const Space& candidate) {
      return AlignUp(candidate.begin, entry->base) + file.bytes.size() <= candidate.end;
    });
    if (space == spaces.end()) {
      plan.left_out.push_back(file.path);
      continue;
    }
    const uint64_t at = AlignUp(space->begin, entry->base);
    space->begin = at + file.bytes.size();
    plan.patches.push_back(CellPatch(entry->size_cell, file.bytes.size()));
    if (entry->extract_size_cell.width) {
      plan.patches.push_back(CellPatch(entry->extract_size_cell, file.bytes.size()));
    }
    plan.patches.push_back(CellPatch(entry->offset_cell, at - entry->base));
    plan.patches.push_back({at, file.bytes});
  }
  return plan;
}

}  // namespace nier
