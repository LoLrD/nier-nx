#include "cpk_toc.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>

namespace nier {

namespace {

constexpr uint32_t kChunkHeaderBytes = 16;
constexpr uint32_t kUtfBase = 8;
constexpr uint32_t kMaxChunkBytes = 64u << 20;
constexpr uint8_t kFlagDefault = 0x20;
constexpr uint8_t kFlagRow = 0x40;
constexpr uint8_t kTypeString = 0xA;

uint64_t Big(std::span<const uint8_t> bytes, size_t at, size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size; ++i) {
    value = value << 8 | bytes[at + i];
  }
  return value;
}

size_t TypeSize(uint8_t type) {
  switch (type) {
    case 0:
    case 1:
      return 1;
    case 2:
    case 3:
      return 2;
    case 6:
    case 7:
    case 9:
    case 0xB:
      return 8;
    case 0xC:
      return 16;
    default:
      return 4;
  }
}

struct Cell {
  uint64_t number = 0;
  std::string text;
  size_t at = 0;
  uint8_t width = 0;
  bool per_row = false;
};

class UtfTable {
 public:
  static std::optional<UtfTable> Parse(std::vector<uint8_t> bytes, uint64_t base) {
    if (bytes.size() < 32) {
      return std::nullopt;
    }
    const bool plain = std::memcmp(bytes.data(), "@UTF", 4) == 0;
    if (!plain) {
      uint8_t key = 0x5F;
      for (uint8_t& byte : bytes) {
        byte ^= key;
        key = uint8_t(key * 0x15);
      }
      if (std::memcmp(bytes.data(), "@UTF", 4) != 0) {
        return std::nullopt;
      }
    }
    UtfTable table;
    table.bytes_ = std::move(bytes);
    table.base_ = base;
    table.plain_ = plain;
    const std::span<const uint8_t> b = table.bytes_;
    table.rows_offset_ = uint32_t(Big(b, 10, 2));
    table.strings_offset_ = uint32_t(Big(b, 12, 4));
    const size_t columns = Big(b, 24, 2);
    table.row_width_ = uint32_t(Big(b, 26, 2));
    table.rows_ = uint32_t(Big(b, 28, 4));
    size_t at = 32;
    size_t per_row_width = 0;
    for (size_t i = 0; i < columns && at + 5 <= b.size(); ++i) {
      Column column{table.String(uint32_t(Big(b, at + 1, 4))), uint8_t(b[at] & 0xF0), uint8_t(b[at] & 0x0F), 0};
      at += 5;
      if (column.flags & kFlagDefault) {
        column.default_at = uint32_t(at);
        at += TypeSize(column.type);
      }
      if (column.flags & kFlagRow) {
        per_row_width += TypeSize(column.type);
      }
      table.columns_.push_back(std::move(column));
    }
    const uint64_t rows_end = kUtfBase + uint64_t(table.rows_offset_) + uint64_t(table.rows_) * table.row_width_;
    if (table.row_width_ < per_row_width || table.rows_ > b.size() || rows_end > b.size()) {
      return std::nullopt;
    }
    return table;
  }

  uint32_t rows() const { return rows_; }

  CpkCell Where(const std::optional<Cell>& cell) const {
    if (!cell || !cell->per_row || !plain_) {
      return {};
    }
    return {base_ + cell->at, cell->width};
  }

  std::optional<Cell> Get(uint32_t row, std::string_view name) const {
    size_t at = kUtfBase + rows_offset_ + size_t(row) * row_width_;
    for (const Column& column : columns_) {
      const bool per_row = column.flags & kFlagRow;
      if (column.name == name) {
        if (per_row) {
          auto cell = Read(at, column.type);
          if (cell) {
            cell->per_row = true;
          }
          return cell;
        }
        if (column.flags & kFlagDefault) {
          return Read(column.default_at, column.type);
        }
        return Cell{};
      }
      if (per_row) {
        at += TypeSize(column.type);
      }
    }
    return std::nullopt;
  }

 private:
  struct Column {
    std::string name;
    uint8_t flags;
    uint8_t type;
    uint32_t default_at;
  };

  std::string String(uint32_t offset) const {
    const size_t start = kUtfBase + strings_offset_ + offset;
    if (start >= bytes_.size()) {
      return {};
    }
    const auto* text = reinterpret_cast<const char*>(bytes_.data() + start);
    return std::string(text, strnlen(text, bytes_.size() - start));
  }

  std::optional<Cell> Read(size_t at, uint8_t type) const {
    const size_t size = TypeSize(type);
    if (at + size > bytes_.size()) {
      return std::nullopt;
    }
    Cell cell;
    cell.at = at;
    cell.width = uint8_t(size);
    if (type == kTypeString) {
      cell.text = String(uint32_t(Big(bytes_, at, 4)));
    } else {
      cell.number = Big(bytes_, at, size);
    }
    return cell;
  }

  std::vector<uint8_t> bytes_;
  std::vector<Column> columns_;
  uint64_t base_ = 0;
  bool plain_ = false;
  uint32_t rows_offset_ = 0;
  uint32_t strings_offset_ = 0;
  uint32_t row_width_ = 0;
  uint32_t rows_ = 0;
};

std::optional<UtfTable> ReadChunk(const CpkReader& read, uint64_t offset, const char* magic) {
  uint8_t header[kChunkHeaderBytes];
  if (!read(offset, header) || std::memcmp(header, magic, 4) != 0) {
    return std::nullopt;
  }
  const uint32_t size = uint32_t(header[8]) | uint32_t(header[9]) << 8 | uint32_t(header[10]) << 16 |
                        uint32_t(header[11]) << 24;
  if (size > kMaxChunkBytes) {
    return std::nullopt;
  }
  std::vector<uint8_t> bytes(size);
  if (!read(offset + kChunkHeaderBytes, bytes)) {
    return std::nullopt;
  }
  return UtfTable::Parse(std::move(bytes), offset + kChunkHeaderBytes);
}

}  // namespace

std::vector<CpkFile> ReadCpkToc(const CpkReader& read) {
  const auto header = ReadChunk(read, 0, "CPK ");
  if (!header || !header->rows()) {
    return {};
  }
  const auto toc_offset = header->Get(0, "TocOffset");
  const auto content_offset = header->Get(0, "ContentOffset");
  if (!toc_offset || !toc_offset->number) {
    return {};
  }
  const auto toc = ReadChunk(read, toc_offset->number, "TOC ");
  if (!toc) {
    return {};
  }
  const uint64_t base =
      content_offset ? std::min(toc_offset->number, content_offset->number) : toc_offset->number;
  std::vector<CpkFile> files;
  for (uint32_t row = 0; row < toc->rows(); ++row) {
    const auto dir = toc->Get(row, "DirName");
    const auto name = toc->Get(row, "FileName");
    const auto size = toc->Get(row, "FileSize");
    const auto extract_size = toc->Get(row, "ExtractSize");
    const auto offset = toc->Get(row, "FileOffset");
    if (!name || !size || !offset) {
      continue;
    }
    const std::string directory = dir && dir->text != "<NULL>" ? dir->text : std::string();
    files.push_back({directory.empty() ? name->text : directory + "/" + name->text, base + offset->number,
                     size->number, base, toc->Where(size), toc->Where(extract_size), toc->Where(offset)});
  }
  return files;
}

}  // namespace nier
