#include "ui.h"

#include <Arduino.h>

#include "board.h"

namespace {
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t LONG_PRESS_MS = 3000;
constexpr uint32_t SHORT_PRESS_MAX_MS = 1000;

constexpr uint32_t BEEP_LEN_MS = 80;
constexpr uint32_t BEEP_ON_BATTERY_MS = 30000;
constexpr uint32_t BEEP_LOW_BATTERY_MS = 5000;
constexpr uint32_t BEEP_FAULT_MS = 2000;
}  // namespace

void Ui::begin() {
  pinMode(brd::PIN_LED_AC, OUTPUT);
  pinMode(brd::PIN_LED_BAT, OUTPUT);
  pinMode(brd::PIN_LED_FAULT, OUTPUT);
  pinMode(brd::PIN_LED_STATUS, OUTPUT);
  pinMode(brd::PIN_BUZZER, OUTPUT);
  pinMode(brd::PIN_BUTTON, INPUT_PULLUP);

  setLed(brd::PIN_LED_AC, false, UPS_EXT_LED_ACTIVE_LOW != 0);
  setLed(brd::PIN_LED_BAT, false, UPS_EXT_LED_ACTIVE_LOW != 0);
  setLed(brd::PIN_LED_FAULT, false, UPS_EXT_LED_ACTIVE_LOW != 0);
  setLed(brd::PIN_LED_STATUS, false, UPS_ONBOARD_LED_ACTIVE_LOW != 0);
  digitalWrite(brd::PIN_BUZZER, UPS_BUZZER_ACTIVE_HIGH ? LOW : HIGH);

  const uint32_t now = millis();
  last_led_ms_ = now;
  last_heartbeat_ms_ = now;
  button_change_ms_ = now;
  button_raw_ = digitalRead(brd::PIN_BUTTON) != LOW;
  button_stable_ = button_raw_;
  button_was_down_ = !button_stable_;
  if (button_was_down_) {
    button_press_ms_ = now;
  }
}

void Ui::poll(uint32_t now_ms, const UiInput& in) {
  // --- button ------------------------------------------------------------
  const bool raw = digitalRead(brd::PIN_BUTTON) != LOW;
  if (raw != button_raw_) {
    button_raw_ = raw;
    button_change_ms_ = now_ms;
  }
  if (button_raw_ != button_stable_ && (now_ms - button_change_ms_) >= DEBOUNCE_MS) {
    button_stable_ = button_raw_;
    if (!button_stable_) {  // pressed
      button_press_ms_ = now_ms;
      long_fired_ = false;
      button_was_down_ = true;
    } else {  // released
      if (button_was_down_ && !long_fired_ &&
          (now_ms - button_press_ms_) < SHORT_PRESS_MAX_MS) {
        short_press_ = true;
      }
      button_was_down_ = false;
    }
  }
  if (button_was_down_ && !long_fired_ &&
      (now_ms - button_press_ms_) >= LONG_PRESS_MS) {
    long_fired_ = true;
    long_press_ = true;
  }

  // --- LEDs --------------------------------------------------------------
  const bool ac_led = in.ac_present;
  setLed(brd::PIN_LED_AC, ac_led, UPS_EXT_LED_ACTIVE_LOW != 0);

  bool bat_led = false;
  if (in.on_battery) {
    if (in.low_battery) {
      bat_led = (now_ms % 1000u) < 250u;
    } else {
      bat_led = true;
    }
  }
  setLed(brd::PIN_LED_BAT, bat_led, UPS_EXT_LED_ACTIVE_LOW != 0);

  bool fault_led = false;
  if (in.fault) {
    fault_led = (now_ms % 500u) < 250u;
  } else if (in.warning) {
    fault_led = (now_ms % 1000u) < 100u;
  }
  setLed(brd::PIN_LED_FAULT, fault_led, UPS_EXT_LED_ACTIVE_LOW != 0);

  bool status_led = false;
  if (in.fault) {
    status_led = (now_ms % 1000u) < 100u;
  } else {
    status_led = (now_ms / 1000u) & 1u;
  }
  setLed(brd::PIN_LED_STATUS, status_led, UPS_ONBOARD_LED_ACTIVE_LOW != 0);

  // --- buzzer ------------------------------------------------------------
  updateBuzzer(now_ms, in);
}

void Ui::updateBuzzer(uint32_t now_ms, const UiInput& in) {
  uint8_t pattern = 0;
  if (in.beeper_enabled) {
    if (in.fault) {
      pattern = 3;
    } else if (in.low_battery && in.on_battery) {
      pattern = 2;
    } else if (in.on_battery) {
      pattern = 1;
    }
  }

  if (pattern != last_pattern_) {
    last_pattern_ = pattern;
    beep_until_ms_ = now_ms;
    if (pattern == 3) {
      next_beep_ms_ = now_ms;
    } else if (pattern == 2) {
      next_beep_ms_ = now_ms;
    } else {
      next_beep_ms_ = now_ms + BEEP_ON_BATTERY_MS;
    }
  }

  if (pattern == 0) {
    digitalWrite(brd::PIN_BUZZER, UPS_BUZZER_ACTIVE_HIGH ? LOW : HIGH);
    return;
  }

  if (static_cast<int32_t>(now_ms - beep_until_ms_) >= 0) {
    digitalWrite(brd::PIN_BUZZER, UPS_BUZZER_ACTIVE_HIGH ? LOW : HIGH);
  }

  if (static_cast<int32_t>(now_ms - next_beep_ms_) >= 0) {
    digitalWrite(brd::PIN_BUZZER, UPS_BUZZER_ACTIVE_HIGH ? HIGH : LOW);
    beep_until_ms_ = now_ms + BEEP_LEN_MS;
    uint32_t interval = BEEP_ON_BATTERY_MS;
    if (pattern == 3) {
      interval = BEEP_FAULT_MS;
    } else if (pattern == 2) {
      interval = BEEP_LOW_BATTERY_MS;
    }
    next_beep_ms_ = now_ms + interval;
  }
}

bool Ui::consumeShortPress() {
  const bool v = short_press_;
  short_press_ = false;
  return v;
}

bool Ui::consumeLongPress() {
  const bool v = long_press_;
  long_press_ = false;
  return v;
}

void Ui::setLed(uint8_t pin, bool on, bool active_low) {
  digitalWrite(pin, (on != active_low) ? HIGH : LOW);
}
