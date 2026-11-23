#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cpk_fixture.h"
#include "cpk_toc.h"
#include "read_patches.h"

using nier::CpkFile;
using nier::ApplyReadPatches;
using nier::Fnv1a64;
using nier::ReadCpkToc;
using nier::ReadPatch;
using nier::TextPatch;

int main() {
  {
    std::vector<uint8_t> data = Bytes("0123456789");
    const std::vector<ReadPatch> patches = {{3, Bytes("abc")}, {8, Bytes("xyz")}};
    ApplyReadPatches(patches, 100, data);
    Check(data == Bytes("0123456789"), "a_read_outside_every_patch_is_left_alone");
    ApplyReadPatches(patches, 0, data);
    Check(data == Bytes("012abc67xy"), "patches_overlapping_a_read_are_applied_and_clipped");
    std::vector<uint8_t> middle = Bytes("4567");
    ApplyReadPatches(patches, 4, middle);
    Check(middle == Bytes("bc67"), "a_read_starting_inside_a_patch_gets_its_tail");
  }
  {
    const std::vector<uint8_t> file = Bytes("1\tPress START to Skip\r\n2\tPress START now\r\n");
    const auto patch = TextPatch(file, 1000, "\tPress START to Skip\r", "\tPress + to Skip    \r");
    Check(patch && patch->begin == 1001 && patch->bytes == Bytes("\tPress + to Skip    \r"),
          "a_unique_text_is_patched_in_place");
    Check(!TextPatch(file, 0, "Press START", "Press +    "), "a_text_found_twice_is_not_patched");
    Check(!TextPatch(file, 0, "Press BACK", "Press -   "), "a_missing_text_is_not_patched");
    Check(!TextPatch(file, 0, "\tPress START to Skip\r", "\tPress + to Skip\r"),
          "a_replacement_of_another_length_is_refused");
  }
  {
    Check(Fnv1a64(Bytes("")) == 0xcbf29ce484222325ull && Fnv1a64(Bytes("a")) == 0xaf63dc4c8601ec8cull,
          "fnv1a64_matches_the_reference_values");
  }
  {
    const std::vector<Column> toc_columns = {{"DirName", 0xA}, {"FileName", 0xA}, {"FileSize", 4}, {"FileOffset", 6}};
    const std::vector<uint8_t> toc = Chunk(
        "TOC ", Utf(toc_columns, {{0, 0, 10, 0x800}, {0, 0, 20, 0x1000}},
                    {{"2D", "COMMON.2DP"}, {"TEXT/INFO_TEXT", "INFOWINDOW.EN.INW"}}));
    const std::vector<Column> header_columns = {{"ContentOffset", 6}, {"TocOffset", 6}};
    std::vector<uint8_t> cpk = Chunk("CPK ", Utf(header_columns, {{0x800, 0x400}}, {{}}));
    cpk.resize(0x400);
    cpk.insert(cpk.end(), toc.begin(), toc.end());
    const auto read = [&](uint64_t offset, std::span<uint8_t> out) {
      if (offset + out.size() > cpk.size()) {
        return false;
      }
      std::memcpy(out.data(), cpk.data() + offset, out.size());
      return true;
    };
    const std::vector<CpkFile> files = ReadCpkToc(read);
    Check(files.size() == 2, "every_toc_row_is_listed");
    Check(files.size() == 2 && files[0].path == "2D/COMMON.2DP" && files[0].offset == 0x400 + 0x800 &&
              files[0].size == 10,
          "offsets_are_relative_to_the_lower_of_toc_and_content");
    Check(files.size() == 2 && files[1].path == "TEXT/INFO_TEXT/INFOWINDOW.EN.INW" && files[1].offset == 0x400 + 0x1000,
          "the_directory_and_name_make_the_path");
    std::vector<uint8_t> broken(16, 0);
    const auto read_broken = [&](uint64_t offset, std::span<uint8_t> out) {
      if (offset + out.size() > broken.size()) {
        return false;
      }
      std::memcpy(out.data(), broken.data() + offset, out.size());
      return true;
    };
    Check(ReadCpkToc(read_broken).empty(), "a_file_that_is_not_a_cpk_has_no_files");
  }
  {
    const std::vector<Column> toc_columns = {{"DirName", 0xA},       {"Extra", 9},       {"Wide", 0xC},
                                             {"FileName", 0xA},      {"FileSize", 4, 0x70, 99},
                                             {"FileOffset", 6}};
    const std::vector<uint8_t> toc =
        Chunk("TOC ", Utf(toc_columns, {{0, 7, 8, 0, 33, 0x800}}, {{"TEXT", "NIER_TEXT.TXD"}}));
    const std::vector<Column> header_columns = {{"ContentOffset", 6, 0x30, 0x400}, {"TocOffset", 6}};
    std::vector<uint8_t> cpk = Chunk("CPK ", Utf(header_columns, {{0, 0x400}}, {{}}));
    cpk.resize(0x400);
    cpk.insert(cpk.end(), toc.begin(), toc.end());
    const auto read = [&](uint64_t offset, std::span<uint8_t> out) {
      if (offset + out.size() > cpk.size()) {
        return false;
      }
      std::memcpy(out.data(), cpk.data() + offset, out.size());
      return true;
    };
    const std::vector<CpkFile> files = ReadCpkToc(read);
    Check(files.size() == 1 && files[0].path == "TEXT/NIER_TEXT.TXD" && files[0].size == 33 &&
              files[0].offset == 0x400 + 0x800,
          "wide_columns_and_default_values_keep_the_row_layout");
  }
  {
    std::vector<uint8_t> huge(16, 0);
    std::memcpy(huge.data(), "CPK ", 4);
    huge[8] = 0x00;
    huge[9] = 0x00;
    huge[10] = 0x00;
    huge[11] = 0xF0;
    uint64_t requested = 0;
    const auto read_huge = [&](uint64_t offset, std::span<uint8_t> out) {
      requested = std::max<uint64_t>(requested, out.size());
      if (offset + out.size() > huge.size()) {
        return false;
      }
      std::memcpy(out.data(), huge.data() + offset, out.size());
      return true;
    };
    Check(ReadCpkToc(read_huge).empty() && requested <= 16, "an_absurd_table_size_is_refused_before_reading_it");
  }
  return failures ? 1 : 0;
}
