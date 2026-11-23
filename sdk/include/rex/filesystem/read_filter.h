#pragma once

#include <cstdint>
#include <span>

namespace rex::filesystem {

class Entry;

using ReadFilter = void (*)(Entry& entry, uint64_t offset, std::span<uint8_t> data);

void SetReadFilter(ReadFilter filter);
ReadFilter GetReadFilter();

}  // namespace rex::filesystem
