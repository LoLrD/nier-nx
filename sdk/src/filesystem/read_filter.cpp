#include <rex/filesystem/read_filter.h>

#include <atomic>

namespace rex::filesystem {

namespace {

std::atomic<ReadFilter> g_read_filter{nullptr};

}  // namespace

void SetReadFilter(ReadFilter filter) { g_read_filter.store(filter, std::memory_order_release); }

ReadFilter GetReadFilter() { return g_read_filter.load(std::memory_order_acquire); }

}  // namespace rex::filesystem
