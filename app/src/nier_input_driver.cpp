#include "nier_input_driver.h"

#include <cstring>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

constexpr auto kScriptDevice = static_cast<rex::input::DeviceId>(0x4E495200);

}  // namespace

NierScriptInputDriver::NierScriptInputDriver(NierInputScript script)
    : InputDriver(nullptr, 0), script_(std::move(script)), start_(std::chrono::steady_clock::now()) {}

rex::X_STATUS NierScriptInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void NierScriptInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kScriptDevice;
  info.name = "NieR input script";
  info.synthetic = true;
  out.push_back(info);
}

rex::X_RESULT NierScriptInputDriver::GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) {
  if (id != kScriptDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const auto since_start =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
  const uint32_t buttons = script_.ButtonsAt(since_start);
  if (buttons != last_buttons_) {
    last_buttons_ = buttons;
    ++packet_number_;
  }
  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = packet_number_;
  out_state->gamepad.buttons = uint16_t(buttons);
  out_state->gamepad.thumb_lx = NierStickAxis(buttons, kNierStickLeft, kNierStickRight);
  out_state->gamepad.thumb_ly = NierStickAxis(buttons, kNierStickDown, kNierStickUp);
  return X_ERROR_SUCCESS;
}

rex::X_RESULT NierScriptInputDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* out_caps) {
  if (id != kScriptDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;
  out_caps->sub_type = 0x01;
  out_caps->gamepad.buttons = 0xFFFF;
  return X_ERROR_SUCCESS;
}

rex::X_RESULT NierScriptInputDriver::SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) {
  return id == kScriptDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

rex::X_RESULT NierScriptInputDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) {
  return id == kScriptDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}
