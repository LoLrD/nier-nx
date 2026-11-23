#include "cpk_toc.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>

namespace nier {

namespace {

constexpr uint32_t kChunkHeaderBytes = 16;
constexpr uint32_t kUtfBase = 8;
constexpr uint8_t kStorageConstant = 0x30;
constexpr uint8_t kStorageRow = 0x50;
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
    case 0xB:
      return 8;
    default:
      return 4;
  }
}

struct Cell {
  uint64_t number = 0;
  std::string text;
};

class UtfTable {
 public:
  static std::optional<UtfTable> Parse(std::vector<uint8_t> bytes) {
    if (bytes.size() < 32) {
      return std::nullopt;
    }
    if (std::memcmp(bytes.data(), "@UTF", 4) != 0) {
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
    const std::span<const uint8_t> b = table.bytes_;
    table.rows_offset_ = uint32_t(Big(b, 8, 4));
    table.strings_offset_ = uint32_t(Big(b, 12, 4));
    const size_t columns = Big(b, 24, 2);
    table.row_width_ = uint32_t(Big(b, 26, 2));
    table.rows_ = uint32_t(Big(b, 28, 4));
    size_t at = 32;
    for (size_t i = 0; i < columns && at + 5 <= b.size(); ++i) {
      Column column{table.String(uint32_t(Big(b, at + 1, 4))), uint8_t(b[at] & 0xF0), uint8_t(b[at] & 0x0F), 0};
      at += 5;
      if (column.storage == kStorageConstant) {
        column.constant_at = uint32_t(at);
        at += TypeSize(column.type);
      }
      table.columns_.push_back(std::move(column));
    }
    return table;
  }

  uint32_t rows() const { return rows_; }

  std::optional<Cell> Get(uint32_t row, std::string_view name) const {
    size_t at = kUtfBase + rows_offset_ + size_t(row) * row_width_;
    for (const Column& column : columns_) {
      const bool per_row = column.storage == kStorageRow;
      if (column.name == name) {
        if (!per_row && column.storage != kStorageConstant) {
          return Cell{};
        }
        return Read(per_row ? at : column.constant_at, column.type);
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
    uint8_t storage;
    uint8_t type;
    uint32_t constant_at;
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
    if (type == kTypeString) {
      cell.text = String(uint32_t(Big(bytes_, at, 4)));
    } else {
      cell.number = Big(bytes_, at, size);
    }
    return cell;
  }

  std::vector<uint8_t> bytes_;
  std::vector<Column> columns_;
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
  std::vector<uint8_t> bytes(size);
  if (!read(offset + kChunkHeaderBytes, bytes)) {
    return std::nullopt;
  }
  return UtfTable::Parse(std::move(bytes));
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
    const auto offset = toc->Get(row, "FileOffset");
    if (!name || !size || !offset) {
      continue;
    }
    const std::string directory = dir && dir->text != "<NULL>" ? dir->text : std::string();
    files.push_back({directory.empty() ? name->text : directory + "/" + name->text, base + offset->number,
                     size->number});
  }
  return files;
}

}  // namespace nier
