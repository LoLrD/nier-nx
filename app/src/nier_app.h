#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
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
    const char* path = std::getenv("NIER_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const auto* image = runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    std::FILE* file = std::fopen(path, "wb");
    std::fwrite(image, 1, PPCImageConfig.image_size, file);
    std::fclose(file);
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
