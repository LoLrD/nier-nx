#include "nier_pgo.h"

#if defined(NIER_PGO_GENERATE)

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {
#include <gcov.h>
}

#include <rex/logging.h>

extern "C" const struct gcov_info* const __start_gcov_info[];
extern "C" const struct gcov_info* const __stop_gcov_info[];

namespace nier::pgo {

namespace {

struct Stream {
  std::FILE* file = nullptr;
  bool ok = true;
  size_t bytes = 0;
  std::vector<std::unique_ptr<unsigned char[]>> scratch;
};

std::jthread g_thread;

void Write(const void* data, unsigned length, void* arg) {
  auto* stream = static_cast<Stream*>(arg);
  if (stream->ok && length && std::fwrite(data, 1, length, stream->file) != length) {
    stream->ok = false;
  }
  stream->bytes += length;
}

void Filename(const char* name, void* arg) { __gcov_filename_to_gcfn(name, Write, arg); }

void* Allocate(unsigned length, void* arg) {
  auto* stream = static_cast<Stream*>(arg);
  stream->scratch.push_back(std::make_unique<unsigned char[]>(length ? length : 1));
  return stream->scratch.back().get();
}

void Dump(const std::filesystem::path& file) {
  std::filesystem::path temporary = file;
  temporary += ".tmp";
  Stream stream;
  stream.file = std::fopen(temporary.string().c_str(), "wb");
  if (!stream.file) {
    REXLOG_WARN("[pgo] cannot open {}", temporary.string());
    return;
  }
  static std::vector<char> buffer(1 << 20);
  std::setvbuf(stream.file, buffer.data(), _IOFBF, buffer.size());
  const gcov_info* const* info = __start_gcov_info;
  const gcov_info* const* const end = __stop_gcov_info;
  __asm__("" : "+r"(info));
  size_t objects = 0;
  for (; info != end && stream.ok; ++info) {
    if (*info) {
      __gcov_info_to_gcda(*info, Filename, Write, Allocate, &stream);
      ++objects;
    }
  }
  const bool closed = std::fclose(stream.file) == 0;
  if (!stream.ok || !closed) {
    REXLOG_WARN("[pgo] writing {} failed, the previous profile stays", temporary.string());
    return;
  }
  std::error_code ec;
  std::filesystem::remove(file, ec);
  std::filesystem::rename(temporary, file, ec);
  REXLOG_INFO("[pgo] {} objects, {} KB written to {}", objects, stream.bytes >> 10, file.string());
}

}  // namespace

void Start(std::string folder, int period_seconds) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::filesystem::path file;
  for (int session = 1; session < 10000; ++session) {
    file = std::filesystem::path(folder) / ("nier_" + std::to_string(session) + ".stream");
    if (!std::filesystem::exists(file, ec)) {
      break;
    }
  }
  REXLOG_INFO("[pgo] training build: profile goes to {} every {} s", file.string(), period_seconds);
  g_thread = std::jthread([file, period_seconds](std::stop_token stop) {
    while (!stop.stop_requested()) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(period_seconds);
      while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      Dump(file);
    }
  });
}

void Stop() {
  g_thread.request_stop();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace nier::pgo

#else

namespace nier::pgo {

void Start(std::string, int) {}
void Stop() {}

}  // namespace nier::pgo

#endif
