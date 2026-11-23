#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "cpk_fixture.h"
#include "cpk_toc.h"
#include "language_patches.h"
#include "read_patches.h"

using nier::ApplyReadPatches;
using nier::CpkFile;
using nier::LanguagePlan;
using nier::PlanLanguage;
using nier::ReadCpkToc;
using nier::TranslatedFile;

constexpr uint64_t kBase = 0x400;

static std::vector<uint8_t> Archive() {
  const std::vector<Column> toc_columns = {{"DirName", 0xA},    {"FileName", 0xA},  {"FileSize", 4},
                                           {"ExtractSize", 4}, {"FileOffset", 6}};
  const std::vector<uint8_t> toc = Chunk(
      "TOC ", Utf(toc_columns,
                  {{0, 0, 0x10, 0x10, 0x800},
                   {0, 0, 20, 20, 0x1000},
                   {0, 0, 0x1800, 0x1800, 0x1800},
                   {0, 0, 0x800, 0x800, 0x3000},
                   {0, 0, 0x10, 0x10, 0x3800}},
                  {{"2D", "COMMON.2DP"},
                   {"TEXT/INFO_TEXT", "INFOWINDOW.EN.INW"},
                   {"TEXT/INFO_TEXT", "INFOWINDOW.FR.INW"},
                   {"TEXT/INFO_TEXT", "INFOWINDOW.DE.INW"},
                   {"2D", "TITLE.2DP"}}));
  const std::vector<Column> header_columns = {{"ContentOffset", 6}, {"TocOffset", 6}};
  std::vector<uint8_t> cpk = Chunk("CPK ", Utf(header_columns, {{0x800, kBase}}, {{}}));
  cpk.resize(kBase);
  cpk.insert(cpk.end(), toc.begin(), toc.end());
  cpk.resize(kBase + 0x3810, 'e');
  return cpk;
}

static std::vector<uint8_t> Pattern(size_t size) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = uint8_t(i * 7 + 1);
  }
  return bytes;
}

int main() {
  const std::vector<uint8_t> cpk = Archive();
  const auto read = [&](uint64_t offset, std::span<uint8_t> out) {
    if (offset + out.size() > cpk.size()) {
      return false;
    }
    std::memcpy(out.data(), cpk.data() + offset, out.size());
    return true;
  };
  const std::vector<CpkFile> files = ReadCpkToc(read);
  {
    const CpkFile& file = files[1];
    Check(file.size_cell.width == 4 && BigAt(cpk, file.size_cell.at, 4) == 20, "the_size_cell_holds_the_file_size");
    Check(file.extract_size_cell.width == 4 && BigAt(cpk, file.extract_size_cell.at, 4) == 20,
          "the_extract_size_cell_holds_the_unpacked_size");
    Check(file.offset_cell.width == 8 && BigAt(cpk, file.offset_cell.at, 8) + file.base == file.offset,
          "the_offset_cell_plus_the_base_is_the_offset");
  }
  {
    const std::vector<uint8_t> russian = Pattern(0x2000);
    const std::vector<TranslatedFile> translated = {{"TEXT/INFO_TEXT/INFOWINDOW.EN.INW", russian}};
    const LanguagePlan plan = PlanLanguage(files, translated);
    const auto read_patched = [&](uint64_t offset, std::span<uint8_t> out) {
      const bool ok = read(offset, out);
      ApplyReadPatches(plan.patches, offset, out);
      return ok;
    };
    const std::vector<CpkFile> moved = ReadCpkToc(read_patched);
    Check(moved.size() == 5 && moved[1].size == russian.size() && (moved[1].offset - moved[1].base) % 0x800 == 0 &&
              moved[1].offset >= kBase + 0x1000 && moved[1].offset + russian.size() <= kBase + 0x3800,
          "a_translation_takes_the_space_of_other_language_texts");
    std::vector<uint8_t> content(russian.size());
    Check(moved.size() == 5 && read_patched(moved[1].offset, content) && content == russian,
          "the_moved_file_reads_back_as_the_translation");
    Check(moved.size() == 5 && moved[0].offset == files[0].offset && moved[4].offset == files[4].offset &&
              moved[4].size == files[4].size,
          "files_outside_the_spare_space_stay_where_they_were");
    std::vector<uint8_t> title(0x10);
    Check(read_patched(files[4].offset, title) && title == std::vector<uint8_t>(0x10, 'e'),
          "the_bytes_of_other_files_are_untouched");
    Check(plan.left_out.empty(), "nothing_is_left_out_when_it_fits");
  }
  {
    const std::vector<uint8_t> glyphs = Pattern(0x10);
    const std::vector<TranslatedFile> translated = {{"2D/TITLE.2DP", glyphs}};
    const LanguagePlan plan = PlanLanguage(files, translated);
    const auto read_patched = [&](uint64_t offset, std::span<uint8_t> out) {
      const bool ok = read(offset, out);
      ApplyReadPatches(plan.patches, offset, out);
      return ok;
    };
    const std::vector<CpkFile> after = ReadCpkToc(read_patched);
    std::vector<uint8_t> content(0x10);
    Check(plan.left_out.empty() && plan.patches.size() == 1 && after.size() == 5 &&
              after[4].offset == files[4].offset && read_patched(files[4].offset, content) && content == glyphs,
          "a_file_of_the_same_size_is_replaced_where_it_is");
  }
  {
    const std::vector<TranslatedFile> translated = {{"TEXT/INFO_TEXT/INFOWINDOW.EN.INW", Pattern(0x3000)}};
    const LanguagePlan plan = PlanLanguage(files, translated);
    Check(plan.patches.empty() && plan.left_out.size() == 1, "a_translation_that_fits_nowhere_keeps_the_english");
  }
  {
    const std::vector<TranslatedFile> translated = {{"TEXT/NOPE.EN", Pattern(8)}};
    const LanguagePlan plan = PlanLanguage(files, translated);
    Check(plan.patches.empty() && plan.left_out.size() == 1, "a_translation_without_a_file_in_the_archive_is_left_out");
  }
  return failures ? 1 : 0;
}
