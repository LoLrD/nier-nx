#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cpk_toc.h"
#include "read_patches.h"

using nier::CpkFile;
using nier::ApplyReadPatches;
using nier::Fnv1a64;
using nier::ReadCpkToc;
using nier::ReadPatch;
using nier::TextPatch;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static std::vector<uint8_t> Bytes(std::string_view text) { return {text.begin(), text.end()}; }

static void Put32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[at + i] = uint8_t(value >> (24 - 8 * i));
  }
}

static void Append32(std::vector<uint8_t>& out, uint32_t value) {
  out.resize(out.size() + 4);
  Put32(out, out.size() - 4, value);
}

static void Append64(std::vector<uint8_t>& out, uint64_t value) {
  Append32(out, uint32_t(value >> 32));
  Append32(out, uint32_t(value));
}

struct Column {
  std::string name;
  uint8_t type;
};

static std::vector<uint8_t> Utf(const std::vector<Column>& columns, const std::vector<std::vector<uint64_t>>& rows,
                                const std::vector<std::vector<std::string>>& strings) {
  std::vector<uint8_t> names;
  std::vector<uint32_t> name_offsets;
  auto add_string = [&](const std::string& text) {
    const uint32_t offset = uint32_t(names.size());
    names.insert(names.end(), text.begin(), text.end());
    names.push_back(0);
    return offset;
  };
  add_string("<NULL>");
  add_string("TABLE");
  for (const Column& column : columns) {
    name_offsets.push_back(add_string(column.name));
  }
  std::vector<uint8_t> schema;
  for (size_t i = 0; i < columns.size(); ++i) {
    schema.push_back(uint8_t(0x50 | columns[i].type));
    Append32(schema, name_offsets[i]);
  }
  std::vector<uint8_t> row_data;
  uint16_t row_width = 0;
  for (size_t r = 0; r < rows.size(); ++r) {
    const size_t start = row_data.size();
    size_t string_index = 0;
    for (size_t c = 0; c < columns.size(); ++c) {
      if (columns[c].type == 0xA) {
        Append32(row_data, add_string(strings[r][string_index++]));
      } else if (columns[c].type == 6) {
        Append64(row_data, rows[r][c]);
      } else {
        Append32(row_data, uint32_t(rows[r][c]));
      }
    }
    row_width = uint16_t(row_data.size() - start);
  }
  std::vector<uint8_t> table(32);
  std::memcpy(table.data(), "@UTF", 4);
  const uint32_t rows_offset = uint32_t(24 + schema.size());
  const uint32_t strings_offset = uint32_t(rows_offset + row_data.size());
  const uint32_t data_offset = uint32_t(strings_offset + names.size());
  Put32(table, 4, data_offset);
  Put32(table, 8, rows_offset);
  Put32(table, 12, strings_offset);
  Put32(table, 16, data_offset);
  Put32(table, 20, 7);
  table[24] = uint8_t(columns.size() >> 8);
  table[25] = uint8_t(columns.size());
  table[26] = uint8_t(row_width >> 8);
  table[27] = uint8_t(row_width);
  Put32(table, 28, uint32_t(rows.size()));
  table.insert(table.end(), schema.begin(), schema.end());
  table.insert(table.end(), row_data.begin(), row_data.end());
  table.insert(table.end(), names.begin(), names.end());
  return table;
}

static std::vector<uint8_t> Chunk(const char* magic, const std::vector<uint8_t>& utf) {
  std::vector<uint8_t> out(16);
  std::memcpy(out.data(), magic, 4);
  out[8] = uint8_t(utf.size());
  out[9] = uint8_t(utf.size() >> 8);
  out[10] = uint8_t(utf.size() >> 16);
  out.insert(out.end(), utf.begin(), utf.end());
  return out;
}

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
  return failures ? 1 : 0;
}
