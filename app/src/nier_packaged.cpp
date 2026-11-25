#include "nier_packaged.h"

#include <rex/rex_app.h>

#if defined(__SWITCH__)

#include <cstdio>
#include <filesystem>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/logging.h>

extern "C" int __system_argc;
extern "C" char** __system_argv;
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

namespace {

constexpr char kMarker[] = "romfs:/nier-nx-package.txt";
constexpr char kSwitchDir[] = "sdmc:/switch";
constexpr char kDataDir[] = "sdmc:/switch/nier-nx";
constexpr char kAnchorText[] =
    "Writable folder of the installed NieR NSP: saves, caches and logs.\n"
    "The game data is inside the NSP. A nier.toml placed here replaces the packaged settings.\n";

bool g_active = false;
char g_anchor[] = "sdmc:/switch/nier-nx/nier-nx-nsp.txt";
char* g_argv[2] = {g_anchor, nullptr};

}  // namespace

extern "C" void __libnx_initheap(void) {
  void* address = nullptr;
  u64 size = 0;
  if (envHasHeapOverride()) {
    address = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    u64 total = 0;
    u64 used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (total > used + 0x200000) {
      size = (total - used - 0x200000) & ~u64(0x1FFFFF);
    }
    if (size == 0) {
      size = 0x2000000 * 16;
    }
    if (size > 0x6000000) {
      size -= 0x6000000;
    }
    if (R_FAILED(svcSetHeapSize(&address, size))) {
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    }
  }
  fake_heap_start = static_cast<char*>(address);
  fake_heap_end = static_cast<char*>(address) + size;
}

extern "C" void userAppInit(void) {
  if (!envIsNso() || R_FAILED(romfsMountSelf("romfs"))) {
    return;
  }
  struct stat st;
  if (stat(kMarker, &st) != 0) {
    romfsUnmount("romfs");
    return;
  }
  mkdir(kSwitchDir, 0777);
  mkdir(kDataDir, 0777);
  chdir(kDataDir);
  if (stat(g_anchor, &st) != 0) {
    if (std::FILE* file = std::fopen(g_anchor, "w")) {
      std::fputs(kAnchorText, file);
      std::fclose(file);
    }
  }
  g_active = true;
  if (__system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0]) {
    __system_argc = 1;
    __system_argv = g_argv;
  }
}

namespace nier::packaged {

bool Active() { return g_active; }

void ConfigurePaths(rex::PathConfig& paths) {
  if (!g_active) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path romfs("romfs:/");
  if (paths.game_data_root.empty() && std::filesystem::is_directory(romfs / "game_root", ec)) {
    paths.game_data_root = romfs / "game_root";
  }
  if (!std::filesystem::exists(paths.config_path, ec)) {
    paths.config_path = romfs / paths.config_path.filename();
  }
  rex::cvar::SetFlagFromCommandLine("vfs_index", "false");
  if (std::filesystem::is_directory(romfs / "marketplace" / "0000000000000000", ec)) {
    rex::cvar::SetFlagFromCommandLine("content_marketplace_root", (romfs / "marketplace").string());
  }
  if (std::filesystem::exists(romfs / "lang" / "files.txt", ec)) {
    rex::cvar::SetFlagFromCommandLine("nier_language_dir", (romfs / "lang").string());
  }
}

void LogStatus() {
  if (!g_active) {
    return;
  }
  REXLOG_INFO("[package] installed NSP: game data from romfs:/, writable folder {}", kDataDir);
}

}  // namespace nier::packaged

#else

namespace nier::packaged {

bool Active() { return false; }
void ConfigurePaths(rex::PathConfig&) {}
void LogStatus() {}

}  // namespace nier::packaged

#endif
