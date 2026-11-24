#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

inline constexpr uint16_t kNierButtonDpadUp = 0x0001;
inline constexpr uint16_t kNierButtonDpadDown = 0x0002;
inline constexpr uint16_t kNierButtonDpadLeft = 0x0004;
inline constexpr uint16_t kNierButtonDpadRight = 0x0008;
inline constexpr uint16_t kNierButtonStart = 0x0010;
inline constexpr uint16_t kNierButtonBack = 0x0020;
inline constexpr uint16_t kNierButtonLeftThumb = 0x0040;
inline constexpr uint16_t kNierButtonRightThumb = 0x0080;
inline constexpr uint16_t kNierButtonLeftShoulder = 0x0100;
inline constexpr uint16_t kNierButtonRightShoulder = 0x0200;
inline constexpr uint16_t kNierButtonA = 0x1000;
inline constexpr uint16_t kNierButtonB = 0x2000;
inline constexpr uint16_t kNierButtonX = 0x4000;
inline constexpr uint16_t kNierButtonY = 0x8000;
inline constexpr uint32_t kNierStickUp = 0x10000;
inline constexpr uint32_t kNierStickDown = 0x20000;
inline constexpr uint32_t kNierStickLeft = 0x40000;
inline constexpr uint32_t kNierStickRight = 0x80000;

int16_t NierStickAxis(uint32_t buttons, uint32_t negative, uint32_t positive);

class NierInputScript {
 public:
  static std::expected<NierInputScript, std::string> Load(const std::filesystem::path& path);

  uint32_t ButtonsAt(std::chrono::milliseconds since_start) const;

 private:
  struct Press {
    std::chrono::milliseconds start;
    std::chrono::milliseconds end;
    uint32_t buttons;
  };

  std::vector<Press> presses_;
};
