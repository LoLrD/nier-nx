#pragma once

#include <chrono>
#include <vector>

#include <rex/input/input_driver.h>

#include "nier_input_script.h"

class NierScriptInputDriver : public rex::input::InputDriver {
 public:
  explicit NierScriptInputDriver(NierInputScript script);

  rex::X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags, rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override;
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags, rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  NierInputScript script_;
  std::chrono::steady_clock::time_point start_;
  uint32_t last_buttons_ = 0;
  uint32_t packet_number_ = 0;
};
