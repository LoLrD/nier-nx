#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

inline int failures = 0;

inline void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

inline std::vector<uint8_t> Bytes(std::string_view text) { return {text.begin(), text.end()}; }

inline void Put32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[at + i] = uint8_t(value >> (24 - 8 * i));
  }
}

inline void Append32(std::vector<uint8_t>& out, uint32_t value) {
  out.resize(out.size() + 4);
  Put32(out, out.size() - 4, value);
}

inline void Append64(std::vector<uint8_t>& out, uint64_t value) {
  Append32(out, uint32_t(value >> 32));
  Append32(out, uint32_t(value));
}

struct Column {
  std::string name;
  uint8_t type;
  uint8_t storage = 0x50;
  uint64_t constant = 0;
};

inline void AppendNumber(std::vector<uint8_t>& out, uint8_t type, uint64_t value) {
  if (type == 6 || type == 9) {
    Append64(out, value);
  } else if (type == 0xC) {
    Append64(out, 0);
    Append64(out, value);
  } else {
    Append32(out, uint32_t(value));
  }
}

inline std::vector<uint8_t> Utf(const std::vector<Column>& columns, const std::vector<std::vector<uint64_t>>& rows,
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
    schema.push_back(uint8_t(columns[i].storage | columns[i].type));
    Append32(schema, name_offsets[i]);
    if (columns[i].storage & 0x20) {
      AppendNumber(schema, columns[i].type, columns[i].constant);
    }
  }
  std::vector<uint8_t> row_data;
  uint16_t row_width = 0;
  for (size_t r = 0; r < rows.size(); ++r) {
    const size_t start = row_data.size();
    size_t string_index = 0;
    for (size_t c = 0; c < columns.size(); ++c) {
      if (!(columns[c].storage & 0x40)) {
        continue;
      }
      if (columns[c].type == 0xA) {
        Append32(row_data, add_string(strings[r][string_index++]));
      } else {
        AppendNumber(row_data, columns[c].type, rows[r][c]);
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
  table[8] = 0;
  table[9] = 1;
  table[10] = uint8_t(rows_offset >> 8);
  table[11] = uint8_t(rows_offset);
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

inline std::vector<uint8_t> Chunk(const char* magic, const std::vector<uint8_t>& utf) {
  std::vector<uint8_t> out(16);
  std::memcpy(out.data(), magic, 4);
  out[8] = uint8_t(utf.size());
  out[9] = uint8_t(utf.size() >> 8);
  out[10] = uint8_t(utf.size() >> 16);
  out.insert(out.end(), utf.begin(), utf.end());
  return out;
}

inline uint64_t BigAt(const std::vector<uint8_t>& bytes, uint64_t at, uint8_t width) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < width; ++i) {
    value = value << 8 | bytes[at + i];
  }
  return value;
}
