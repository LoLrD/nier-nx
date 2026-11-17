#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

inline std::string_view TrimNierControl(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

inline std::vector<std::pair<std::string, std::string>> ParseNierControl(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> settings;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    const auto line = TrimNierControl(text.substr(0, newline));
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    const auto equals = line.find('=');
    if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
      continue;
    }
    const auto key = TrimNierControl(line.substr(0, equals));
    const auto value = TrimNierControl(line.substr(equals + 1));
    if (!key.empty()) {
      settings.emplace_back(std::string(key), std::string(value));
    }
  }
  return settings;
}
