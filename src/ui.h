#pragma once

#include <stdint.h>

#include "telemetry.h"

struct UiInput {
  ups::State state;
  bool ac_present;
  bool on_battery;
  bool low_battery;
  bool warning;
  bool fault;
  bool test_active;
  bool beeper_enabled;
};

class Ui {
 public:
  void begin();
  void poll(uint32_t now_ms, const UiInput& in);

  bool consumeShortPress();
  bool consumeLongPress();

 private:
  void setLed(uint8_t pin, bool on, bool active_low);
  void updateBuzzer(uint32_t now_ms, const UiInput& in);

  bool short_press_ = false;
  bool long_press_ = false;

  bool button_raw_ = true;
  bool button_stable_ = true;
  uint32_t button_change_ms_ = 0;
  uint32_t button_press_ms_ = 0;
  bool long_fired_ = false;
  bool button_was_down_ = false;

  uint8_t led_phase_ = 0;
  uint32_t last_led_ms_ = 0;
  uint32_t last_heartbeat_ms_ = 0;
  bool heartbeat_ = false;

  uint32_t next_beep_ms_ = 0;
  uint32_t beep_until_ms_ = 0;
  uint8_t last_pattern_ = 0xFF;
};
