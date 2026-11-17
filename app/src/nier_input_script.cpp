#include "nier_input_script.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

constexpr std::array<std::pair<std::string_view, uint16_t>, 14> kButtonNames{{
    {"DPAD_UP", kNierButtonDpadUp},
    {"DPAD_DOWN", kNierButtonDpadDown},
    {"DPAD_LEFT", kNierButtonDpadLeft},
    {"DPAD_RIGHT", kNierButtonDpadRight},
    {"START", kNierButtonStart},
    {"BACK", kNierButtonBack},
    {"LEFT_THUMB", kNierButtonLeftThumb},
    {"RIGHT_THUMB", kNierButtonRightThumb},
    {"LB", kNierButtonLeftShoulder},
    {"RB", kNierButtonRightShoulder},
    {"A", kNierButtonA},
    {"B", kNierButtonB},
    {"X", kNierButtonX},
    {"Y", kNierButtonY},
}};

std::optional<int64_t> ParseMilliseconds(std::string_view text) {
  int64_t value = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size() || value < 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<uint16_t> ParseButtons(std::string_view text) {
  uint16_t buttons = 0;
  while (!text.empty()) {
    const auto plus = text.find('+');
    const auto name = text.substr(0, plus);
    auto it = std::find_if(kButtonNames.begin(), kButtonNames.end(), [&](const auto& entry) { return entry.first == name; });
    if (it == kButtonNames.end()) {
      return std::nullopt;
    }
    buttons |= it->second;
    text = plus == std::string_view::npos ? std::string_view{} : text.substr(plus + 1);
  }
  return buttons == 0 ? std::nullopt : std::optional<uint16_t>(buttons);
}

}  // namespace

std::expected<NierInputScript, std::string> NierInputScript::Load(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return std::unexpected("cannot open input script " + path.string());
  }
  NierInputScript script;
  std::string line;
  for (int number = 1; std::getline(file, line); ++number) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::array<std::string_view, 3> fields;
    std::string_view rest = line;
    size_t count = 0;
    while (count < fields.size()) {
      const auto tab = rest.find('\t');
      fields[count++] = rest.substr(0, tab);
      if (tab == std::string_view::npos) {
        rest = {};
        break;
      }
      rest = rest.substr(tab + 1);
    }
    const auto start = count == 3 && rest.empty() ? ParseMilliseconds(fields[0]) : std::nullopt;
    const auto buttons = start ? ParseButtons(fields[1]) : std::nullopt;
    const auto duration = buttons ? ParseMilliseconds(fields[2]) : std::nullopt;
    if (!duration) {
      return std::unexpected("bad input script line " + std::to_string(number));
    }
    script.presses_.push_back({std::chrono::milliseconds(*start), std::chrono::milliseconds(*start + *duration), *buttons});
  }
  return script;
}

uint16_t NierInputScript::ButtonsAt(std::chrono::milliseconds since_start) const {
  uint16_t buttons = 0;
  for (const auto& press : presses_) {
    if (since_start >= press.start && since_start < press.end) {
      buttons |= press.buttons;
    }
  }
  return buttons;
}
