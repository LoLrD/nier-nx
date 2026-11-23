#include "nier_archive_patches.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/read_filter.h>
#include <rex/logging.h>

#include "cpk_toc.h"
#include "language_patches.h"
#include "nier_language.h"
#include "nier_switch_prompts.h"
#include "read_patches.h"

REXCVAR_DECLARE(bool, nier_switch_prompts);
REXCVAR_DECLARE(std::string, nier_language_dir);

namespace nier {

namespace {

bool EndsWithCpk(std::string_view name) {
  return name.size() >= 4 && std::equal(name.end() - 4, name.end(), ".cpk", [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == b;
         });
}

void Append(std::vector<ReadPatch>& to, std::vector<ReadPatch> from) {
  to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

class ArchivePatches {
 public:
  void Filter(rex::filesystem::Entry& entry, uint64_t offset, std::span<uint8_t> data) {
    if (!EndsWithCpk(entry.name())) {
      return;
    }
    const std::vector<ReadPatch>* patches = nullptr;
    {
      std::lock_guard lock(mutex_);
      auto it = patches_.find(&entry);
      if (it == patches_.end()) {
        auto built = Build(entry);
        if (!built) {
          return;
        }
        it = patches_.emplace(&entry, std::move(*built)).first;
      }
      patches = &it->second;
    }
    ApplyReadPatches(*patches, offset, data);
  }

 private:
  static std::optional<std::vector<ReadPatch>> Build(rex::filesystem::Entry& entry) {
    rex::filesystem::File* file = nullptr;
    if (XFAILED(entry.Open(rex::filesystem::FileAccess::kGenericRead, &file))) {
      REXLOG_WARN("[archive] could not open {}, will try again", entry.path());
      return std::nullopt;
    }
    bool failed = false;
    const auto read = [file, &failed](uint64_t offset, std::span<uint8_t> out) {
      size_t done = 0;
      const bool ok = XSUCCEEDED(file->ReadSync(out, size_t(offset), &done)) && done == out.size();
      failed |= !ok;
      return ok;
    };
    const std::vector<CpkFile> toc = ReadCpkToc(read);
    std::vector<ReadPatch> result;
    if (REXCVAR_GET(nier_switch_prompts)) {
      Append(result, SwitchPromptPatches(toc, read, entry.path()));
    }
    const std::vector<TranslatedFile> translated = LoadTranslatedFiles(toc);
    if (!translated.empty()) {
      LanguagePlan plan = PlanLanguage(toc, translated);
      Append(result, std::move(plan.patches));
      REXLOG_INFO("[language] {}: {} of {} translated files placed", entry.path(),
                  translated.size() - plan.left_out.size(), translated.size());
      for (const std::string& path : plan.left_out) {
        REXLOG_WARN("[language] {} does not fit, it stays in English", path);
      }
    }
    file->Destroy();
    if (failed) {
      REXLOG_WARN("[archive] could not read {}, will try again", entry.path());
      return std::nullopt;
    }
    return result;
  }

  std::mutex mutex_;
  std::unordered_map<const rex::filesystem::Entry*, std::vector<ReadPatch>> patches_;
};

ArchivePatches& Patches() {
  static ArchivePatches patches;
  return patches;
}

void Filter(rex::filesystem::Entry& entry, uint64_t offset, std::span<uint8_t> data) {
  Patches().Filter(entry, offset, data);
}

}  // namespace

void InstallArchivePatches() {
  if (REXCVAR_GET(nier_switch_prompts) || !REXCVAR_GET(nier_language_dir).empty()) {
    rex::filesystem::SetReadFilter(Filter);
  }
}

}  // namespace nier
