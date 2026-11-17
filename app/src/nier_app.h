#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/memory.h>
#include <rex/rex_app.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <rex/runtime.h>

#include "nier_control_file.h"
#include "nier_fps_meter.h"
#include "nier_input_driver.h"
#include "nier_input_script.h"

REXCVAR_DECLARE(std::string, nier_input_script);

class NierApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NierApp>(new NierApp(ctx, "nier", PPCImageConfig));
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

  void WatchControlFile() {
    const char* path = std::getenv("NIER_CONTROL_FILE");
    if (!path) {
      return;
    }
    control_thread_ = std::jthread([this, file = std::string(path)](std::stop_token stop) {
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
            rex::cvar::SetFlagByName(key, value);
            std::fprintf(stderr, "[control] %s = %s\n", key.c_str(), value.c_str());
          }
          applied = text;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
  }

  void OnPostSetup() override {
    WatchControlFile();
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
};
