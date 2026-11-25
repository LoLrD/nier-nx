#include "nier_guest_jmp.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/logging.h>

namespace {

constexpr uint32_t kProtectedCallFrame = 96;
constexpr uint32_t kProtectedCallState = 1492;
constexpr uint32_t kStateTop = 8;
constexpr uint32_t kValueSize = 16;
constexpr uint32_t kValueType = 0;
constexpr uint32_t kValuePointer = 8;
constexpr uint32_t kTypeString = 4;
constexpr uint32_t kStringLength = 12;
constexpr uint32_t kStringText = 16;
constexpr uint32_t kMessageLimit = 400;

thread_local std::unordered_map<uint32_t, NierGuestJmp> g_buffers;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

std::string ScriptError(const uint8_t* base, uint32_t guest_address) {
  const uint32_t state = Load32(base, guest_address - kProtectedCallFrame + kProtectedCallState);
  const uint32_t value = Load32(base, state + kStateTop) - kValueSize;
  if (Load32(base, value + kValueType) != kTypeString) {
    return "(no message)";
  }
  const uint32_t text = Load32(base, value + kValuePointer);
  const uint32_t length = Load32(base, text + kStringLength);
  return std::string(reinterpret_cast<const char*>(base + text + kStringText), length < kMessageLimit ? length : kMessageLimit);
}

}  // namespace

NierGuestJmp& NierGuestJmpArm(uint32_t guest_address) {
  NierGuestJmp& jump = g_buffers[guest_address];
  jump.armed = true;
  return jump;
}

void NierGuestLongjmp(const uint8_t* base, uint32_t guest_address, int value) {
  const auto found = g_buffers.find(guest_address);
  if (found == g_buffers.end() || !found->second.armed) {
    REXLOG_WARN("[jmp] longjmp to {:08X} without a setjmp, code {}", guest_address, value);
    return;
  }
  REXLOG_WARN("[lua] script error {}: {}", value, ScriptError(base, guest_address));
  std::longjmp(found->second.buffer, value);
}
