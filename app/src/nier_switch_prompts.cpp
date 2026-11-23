#include "nier_switch_prompts.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "nier_switch_prompts_data.h"

REXCVAR_DEFINE_BOOL(nier_switch_prompts, true, "NieR",
                    "Show Nintendo Switch button icons and button names instead of the Xbox 360 ones");

namespace nier {

namespace {

struct ImageEdit {
  std::string_view file;
  uint32_t offset;
  uint32_t size;
  uint64_t original_hash;
  std::span<const switch_prompts::Block> blocks;
};

struct TextEdit {
  std::string_view file;
  std::string_view original;
  std::string_view replacement;
};

constexpr uint64_t kAtlasHash = 0xF9C6FC9901275769ull;
constexpr uint64_t kTitleEnHash = 0xF2B0D36DE11E3B7Aull;
constexpr uint64_t kTitleDeHash = 0x1CF1038C180C8391ull;
constexpr uint64_t kTitleFrHash = 0x476A087694E0C732ull;
constexpr uint64_t kTitlePushButtonHash = 0xBC38885DEA75F8C0ull;
constexpr uint64_t kSelectEnHash = 0x62FD495A20BE9B70ull;
constexpr uint64_t kSelectDeHash = 0x64A87EDFD95FA483ull;
constexpr uint64_t kSelectFrHash = 0x646402D17071D225ull;
constexpr uint64_t kSelectPushButtonHash = 0x6BDA9DDAFF550A33ull;

constexpr ImageEdit kImageEdits[] = {
    {"2D/COMMON.2DP", 0x60000, 0x8000, kAtlasHash, switch_prompts::kIconAtlas},
    {"2D/COMMON_DE.2DP", 0x60000, 0x8000, kAtlasHash, switch_prompts::kIconAtlas},
    {"2D/COMMON_FR.2DP", 0x60000, 0x8000, kAtlasHash, switch_prompts::kIconAtlas},
    {"2D/SYSTEM_MENU.2DP", 0x74000, 0x4000, 0x2ED684B62353F707ull, switch_prompts::kSkillL},
    {"2D/SYSTEM_MENU.2DP", 0x78000, 0x4000, 0x0B1C7E718088C4B8ull, switch_prompts::kSkillZL},
    {"2D/SYSTEM_MENU.2DP", 0x7C000, 0x4000, 0xDA5ACC3DF9ACAC38ull, switch_prompts::kSkillR},
    {"2D/SYSTEM_MENU.2DP", 0x80000, 0x4000, 0xD68E7FE277869154ull, switch_prompts::kSkillZR},
    {"2D/TITLE.2DP", 0x50000, 0xC000, kTitleEnHash, switch_prompts::kTitleEn},
    {"2D/TITLE_GESTALT_US.2DP", 0x62000, 0xC000, kTitleEnHash, switch_prompts::kTitleEn},
    {"2D/TITLE_DE.2DP", 0x5C000, 0xC000, kTitleDeHash, switch_prompts::kTitleDe},
    {"2D/TITLE_GESTALT_DE.2DP", 0x6E000, 0xC000, kTitleDeHash, switch_prompts::kTitleDe},
    {"2D/TITLE_FR.2DP", 0x5C000, 0x8000, kTitleFrHash, switch_prompts::kTitleFr},
    {"2D/TITLE_GESTALT_FR.2DP", 0x6E000, 0x8000, kTitleFrHash, switch_prompts::kTitleFr},
    {"2D/TITLE_GESTALT.2DP", 0x62000, 0x8000, 0x73449501E96E18F7ull, switch_prompts::kTitlePush},
    {"2D/TITLE_JP.2DP", 0x50000, 0x8000, kTitlePushButtonHash, switch_prompts::kTitlePushButton},
    {"2D/TITLE_REPLICANT.2DP", 0x7C000, 0x8000, kTitlePushButtonHash, switch_prompts::kTitlePushButton},
    {"2D/TITLE.2DP", 0x5C000, 0x8000, kSelectEnHash, switch_prompts::kSelectEn},
    {"2D/TITLE_GESTALT_US.2DP", 0x6E000, 0x8000, kSelectEnHash, switch_prompts::kSelectEn},
    {"2D/TITLE_DE.2DP", 0x68000, 0x8000, kSelectDeHash, switch_prompts::kSelectDe},
    {"2D/TITLE_GESTALT_DE.2DP", 0x7A000, 0x8000, kSelectDeHash, switch_prompts::kSelectDe},
    {"2D/TITLE_FR.2DP", 0x64000, 0x8000, kSelectFrHash, switch_prompts::kSelectFr},
    {"2D/TITLE_GESTALT_FR.2DP", 0x76000, 0x8000, kSelectFrHash, switch_prompts::kSelectFr},
    {"2D/TITLE_GESTALT.2DP", 0x6A000, 0x4000, 0x558B4C6E7A7C0E9Dull, switch_prompts::kSelectPush},
    {"2D/TITLE_JP.2DP", 0x58000, 0x8000, kSelectPushButtonHash, switch_prompts::kSelectPushButton},
    {"2D/TITLE_REPLICANT.2DP", 0x84000, 0x8000, kSelectPushButtonHash, switch_prompts::kSelectPushButton},
};

constexpr TextEdit kTextEdits[] = {
    {"TEXT/NIER_TEXT.TXD",
     "1112500\tPush Start\tPress the START Button\tAppuyez sur START\tDrücke die START-Taste\r",
     "1112500\tPush <ICON=13>  \tPress <ICON=13> \tAppuyez sur <ICON=13> \tDrücke <ICON=13> \r"},
    {"TEXT/NIER_TEXT.TXD",
     "1500400\tPress START button\tPress the START button\tAppuyez sur la touche START\tBitte die START-Taste drücken\r",
     "1500400\tPress <ICON=13> button  \tPress <ICON=13> \tAppuyez sur la touche <ICON=13> \tBitte <ICON=13> drücken \r"},
    {"TEXT/NIER_TEXT.TXD",
     "11202000\tPush START button to Skip\tPress START to Skip\tAppuyez sur START pour passer la scène\tDrücke START zum Überspringen\r",
     "11202000\tPush <ICON=13> to Skip \tPress <ICON=13> to Skip\tAppuyez sur <ICON=13> pour passer\tDrücke <ICON=13> zum Überspringen\r"},
    {"TEXT/INFO_TEXT/INFOWINDOW.EN.INW",
     "Press the BACK button to view maps you've already obtained.",
     "Press <ICON=12> to view maps you've already obtained.      "},
    {"TEXT/INFO_TEXT/INFOWINDOW.FR.INW",
     "Appuyez sur la touche BACK pour consulter la liste des cartes que vous avez acquises.",
     "Appuyez sur <ICON=12> pour consulter la liste des cartes que vous avez acquises.     "},
    {"TEXT/INFO_TEXT/INFOWINDOW.DE.INW",
     "Drücke die BACK-Taste, um Karten zu sehen, die du bereits erhalten hast.",
     "Drücke <ICON=12>, um Karten zu sehen, die du bereits erhalten hast.     "},
    {"TEXT/INFO_TEXT/INFOWINDOW.JA.INW",
     "入手した地図は BACK ボタンで表示可能です。",
     "入手した地図は <ICON=12> で表示可能です。    "},
};

}  // namespace

std::vector<ReadPatch> SwitchPromptPatches(std::span<const CpkFile> toc, const CpkReader& read,
                                           const std::string& archive) {
  std::vector<ReadPatch> patches;
  uint32_t icons = 0;
  uint32_t texts = 0;
  uint32_t skipped = 0;
  for (const CpkFile& cpk_file : toc) {
    for (const ImageEdit& edit : kImageEdits) {
      if (cpk_file.path != edit.file) {
        continue;
      }
      std::vector<uint8_t> bytes(edit.size);
      if (!read(cpk_file.offset + edit.offset, bytes) || Fnv1a64(bytes) != edit.original_hash) {
        ++skipped;
        continue;
      }
      for (const switch_prompts::Block& block : edit.blocks) {
        std::memcpy(bytes.data() + block.offset, block.bytes, sizeof(block.bytes));
      }
      patches.push_back({cpk_file.offset + edit.offset, std::move(bytes)});
      ++icons;
    }
    std::vector<uint8_t> content;
    for (const TextEdit& edit : kTextEdits) {
      if (cpk_file.path != edit.file) {
        continue;
      }
      if (content.empty()) {
        content.resize(size_t(cpk_file.size));
        if (!read(cpk_file.offset, content)) {
          content.clear();
        }
      }
      if (auto patch = TextPatch(content, cpk_file.offset, edit.original, edit.replacement)) {
        patches.push_back(std::move(*patch));
        ++texts;
      } else {
        ++skipped;
      }
    }
  }
  REXLOG_INFO("[switch_prompts] {}: {} icon patches, {} text patches, {} left as they were", archive, icons, texts,
              skipped);
  return patches;
}

}  // namespace nier
