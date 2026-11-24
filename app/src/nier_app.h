#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/rex_app.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <rex/runtime.h>

#include "nier_control_file.h"
#include "nier_fps_meter.h"
#include "nier_input_driver.h"
#include "nier_input_script.h"
#include "nier_archive_patches.h"
#include "native/native_graphics_system.h"

REXCVAR_DECLARE(std::string, nier_input_script);
REXCVAR_DECLARE(std::string, nier_renderer);
REXCVAR_DECLARE(std::string, nier_control_file);
REXCVAR_DECLARE(std::string, nier_ab_flag);
REXCVAR_DECLARE(std::string, nier_ab_values);
REXCVAR_DECLARE(int32_t, nier_ab_period_s);

class NierApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NierApp>(new NierApp(ctx, "nier", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    nier::InstallArchivePatches();
    if (REXCVAR_GET(nier_renderer) == "native") {
      config.graphics = nier::native::CreateNativeGraphicsSystem();
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto extracted = rex::filesystem::GetExecutableFolder() / "game_root";
    if (std::filesystem::is_directory(extracted, ec)) {
      paths.game_data_root = extracted;
    }
  }

  void OnPostLoadXexImage() override {
    DumpImage();
    ProbeDlc();
  }

  void DumpImage() {
    const char* path = std::getenv("NIER_DUMP_IMAGE");
    if (!path) {
      return;
    }
    std::FILE* file = std::fopen(path, "wb");
    if (!file) {
      std::fprintf(stderr, "cannot write NIER_DUMP_IMAGE %s\n", path);
      std::exit(2);
    }
    const auto* image = runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    std::fwrite(image, 1, PPCImageConfig.image_size, file);
    std::fclose(file);
  }

  void ProbeDlc() {
    const char* path = std::getenv("NIER_PROBE_DLC");
    if (!path) {
      return;
    }
    rex::filesystem::StfsContainerDevice device("\\Device\\NierProbe\\", path);
    if (!device.Initialize()) {
      std::fprintf(stderr, "[dlc-probe] cannot mount %s\n", path);
      return;
    }
    const auto* root = device.ResolvePath("");
    std::fprintf(stderr, "[dlc-probe] mounted %s: %zu root entries\n", path, root->child_count());
    for (const auto& child : root->children()) {
      std::fprintf(stderr, "[dlc-probe]   %s (%zu bytes, %zu children)\n", child->name().c_str(), child->size(),
                   child->child_count());
    }
  }

  void CaptureFrame(const std::string& path) {
    rex::ui::RawImage image;
    if (!runtime()->graphics_system()->presenter()->CaptureGuestOutput(image)) {
      std::fprintf(stderr, "[control] capture failed\n");
      return;
    }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << image.width << " " << image.height << "\n255\n";
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        out.write(reinterpret_cast<const char*>(row + x * 4), 3);
      }
    }
    std::fprintf(stderr, "[control] captured %s (%ux%u)\n", path.c_str(), image.width, image.height);
  }

  void DumpMemory(const std::string& path) {
    auto* memory = runtime()->memory();
    std::ofstream out(path, std::ios::binary);
    uint64_t address = 0;
    uint64_t written = 0;
    while (address < 0xC0000000ull) {
      auto* heap = memory->LookupHeap(uint32_t(address));
      rex::memory::HeapAllocationInfo info{};
      if (!heap || !heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
        address += 0x10000;
        continue;
      }
      if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead)) {
        const uint32_t header[2] = {info.base_address, info.region_size};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        out.write(memory->TranslateVirtual<const char*>(info.base_address), info.region_size);
        written += info.region_size;
      }
      address = uint64_t(info.base_address) + info.region_size;
    }
    std::fprintf(stderr, "[control] dumped %llu MB of guest memory to %s\n",
                 static_cast<unsigned long long>(written >> 20), path.c_str());
  }

  void WatchControlFile() {
    const char* env = std::getenv("NIER_CONTROL_FILE");
    const std::string path = env ? env : REXCVAR_GET(nier_control_file);
    if (path.empty()) {
      return;
    }
    control_thread_ = std::jthread([this, file = path](std::stop_token stop) {
      std::string applied;
      while (!stop.stop_requested()) {
        std::ifstream in(file);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text != applied) {
          for (const auto& [key, value] : ParseNierControl(text)) {
            if (key == "capture") {
              CaptureFrame(value);
              continue;
            }
            if (key == "dump_memory") {
              DumpMemory(value);
              continue;
            }
            rex::cvar::SetFlagByName(key, value);
            std::fprintf(stderr, "[control] %s = %s\n", key.c_str(), value.c_str());
          }
          applied = text;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
  }

  void RunAbSchedule() {
    const std::string flag = REXCVAR_GET(nier_ab_flag);
    std::vector<std::string> values = SplitNierValues(REXCVAR_GET(nier_ab_values));
    if (flag.empty() || values.empty()) {
      return;
    }
    const uint64_t period_ms = uint64_t(std::max(1, REXCVAR_GET(nier_ab_period_s))) * 1000;
    ab_thread_ = std::jthread([flag, values = std::move(values), period_ms](std::stop_token stop) {
      const auto start = std::chrono::steady_clock::now();
      size_t applied = values.size();
      while (!stop.stop_requested()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        const size_t index = NierAbIndex(uint64_t(elapsed.count()), period_ms, values.size());
        if (index != applied) {
          rex::cvar::SetFlagByName(flag, values[index]);
          REXLOG_INFO("[ab] {} = {}", flag, values[index]);
          applied = index;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }

  void OnPostSetup() override {
    WatchControlFile();
    RunAbSchedule();
    SetGuestFrameStats([this] {
      const auto sample = fps_meter_.Update(rex::kernel::xboxkrnl::GuestSwapCount(), std::chrono::steady_clock::now());
      return rex::ui::FrameStats{sample.frame_time_ms, sample.fps, sample.frames};
    });
    const std::string path = REXCVAR_GET(nier_input_script);
    if (path.empty()) {
      return;
    }
    auto script = NierInputScript::Load(path);
    if (!script) {
      std::fprintf(stderr, "%s\n", script.error().c_str());
      std::exit(2);
    }
    auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    input->AddDriver(std::make_unique<NierScriptInputDriver>(std::move(*script)));
  }
 private:
  NierFpsMeter fps_meter_;
  std::jthread control_thread_;
  std::jthread ab_thread_;
};
