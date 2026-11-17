#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/input/input_system.h>
#include <rex/memory.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

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

  void OnPostSetup() override {
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
};
