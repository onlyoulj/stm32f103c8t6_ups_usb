#pragma once

#include <stdint.h>

#include "settings.h"

/*
 * Battery state-of-charge model.
 *
 * Primary method: coulomb counting from the INA219 battery current.
 * Secondary method: open-circuit-voltage correction when the pack has been at
 * rest, and a slow OCV blend when no current sensor is available.
 *
 * This is a supervisory estimate, not a fuel-gauge guarantee.  The BMS remains
 * responsible for per-cell protection and balancing.
 */
class BatteryModel {
 public:
  void begin(const Settings& s, uint16_t saved_soc_x10, bool saved_valid);

  void update(const Settings& s,
              int32_t vbat_mv,
              int32_t ibat_ma,
              bool current_valid,
              bool ac_present,
              bool charging,
              bool charge_done,
              bool discharging,
              uint32_t dt_ms);

  uint16_t socX10() const { return soc_x10_; }
  void setSocX10(uint16_t value);

  bool low() const { return low_; }
  bool critical() const { return critical_; }
  bool full() const { return full_; }

  uint32_t remaining_mAh(const Settings& s) const;
  uint32_t runtimeSeconds(const Settings& s, int32_t load_ma) const;

  static uint16_t ocvSocX10(const Settings& s, int32_t vbat_mv);

 private:
  void integrate(const Settings& s, int32_t ibat_ma, uint32_t dt_ms);
  void updateFlags(const Settings& s, int32_t vbat_mv, bool discharging,
                   uint32_t dt_ms);

  uint16_t soc_x10_;          // 0..1000 (0.1 %)
  int64_t coulomb_rem_;       // uA*s remainder
  bool have_soc_;
  bool low_;
  bool critical_;
  bool full_;

  uint32_t rest_ms_;
  uint32_t full_hold_ms_;
  uint32_t low_voltage_ms_;
  uint32_t critical_voltage_ms_;
  uint32_t ocv_tick_ms_;
};
