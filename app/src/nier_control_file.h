#pragma once

#include <cstddef>
#include <cstdint>
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

inline std::vector<std::string> SplitNierValues(std::string_view text) {
  std::vector<std::string> values;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto value = TrimNierControl(text.substr(0, comma));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    if (!value.empty()) {
      values.emplace_back(value);
    }
  }
  return values;
}

inline size_t NierAbIndex(uint64_t elapsed_ms, uint64_t period_ms, size_t count) {
  return period_ms && count ? size_t(elapsed_ms / period_ms % count) : 0;
}
