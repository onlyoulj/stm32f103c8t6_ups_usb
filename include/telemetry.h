#pragma once

#include <stdint.h>

namespace ups {

enum class State : uint8_t {
  Boot = 0,
  Online,
  OnBattery,
  LowBattery,
  Shutdown,
  Fault,
};

enum Fault : uint16_t {
  FAULT_NONE             = 0,
  FAULT_BATTERY_UV       = 1u << 0,  // pack critically empty
  FAULT_BATTERY_OV       = 1u << 1,  // pack over-voltage (charge inhibited)
  FAULT_BATTERY_MISSING  = 1u << 2,
  FAULT_OVER_CURRENT     = 1u << 3,
  FAULT_OVER_TEMP        = 1u << 4,
  FAULT_OUTPUT_UV        = 1u << 5,
  FAULT_OUTPUT_OV        = 1u << 6,
  FAULT_INPUT_OV         = 1u << 7,
  FAULT_SENSOR           = 1u << 8,
  FAULT_CHARGER          = 1u << 9,
  FAULT_INTERNAL         = 1u << 10,
};

enum Warning : uint16_t {
  WARN_NONE              = 0,
  WARN_BATTERY_OV        = 1u << 0,
  WARN_TEMP_CHARGE       = 1u << 1,
  WARN_TEMP_DISCHARGE    = 1u << 2,
  WARN_CHARGER_STALE     = 1u << 3,
  WARN_CURRENT_SENSOR    = 1u << 4,
  WARN_TEMP_SENSOR       = 1u << 5,
  WARN_OUTPUT_SENSOR     = 1u << 6,
  WARN_CONFIG_INVALID    = 1u << 7,
  WARN_BATTERY_MISSING   = 1u << 8,
  WARN_AC_LOW            = 1u << 9,
  WARN_CHARGE_OVERCURRENT = 1u << 10,
  WARN_BATTERY_SENSOR    = 1u << 11,
  WARN_NVM_ERROR         = 1u << 12,
};

// Bit flags for Telemetry.flags
enum TelemetryFlag : uint8_t {
  TF_AC_PRESENT       = 1u << 0,
  TF_OUTPUT_ON        = 1u << 1,
  TF_CHARGING         = 1u << 2,
  TF_CHARGE_DONE      = 1u << 3,
  TF_BATTERY_PRESENT  = 1u << 4,
  TF_TEST_ACTIVE      = 1u << 5,
  TF_SHUTDOWN_PENDING = 1u << 6,
  TF_BEEPER_ENABLED   = 1u << 7,
};

struct Telemetry {
  uint32_t uptime_s;
  uint32_t ac_fail_count;
  uint32_t runtime_s;
  uint32_t shutdown_remaining_s;
  uint32_t restore_remaining_s;

  int32_t vin_mv;
  int32_t vin_fault_mv;  // Megatec "input fault voltage" glitch value
  int32_t vout_mv;
  int32_t vbat_mv;
  int32_t ibat_ma;   // positive = charging
  int32_t iout_ma;

  int16_t temp_c_x10;
  uint16_t soc_x10;  // 0..1000
  uint16_t load_pct;
  uint16_t faults;
  uint16_t warnings;
  uint8_t state;
  uint8_t flags;

  bool battery_low;
  bool critical_battery;
  bool ina_ok;
  bool ntc_ok;
  bool output_sensor_ok;
  bool config_dirty;
};

inline bool telemetryFlag(const Telemetry& t, TelemetryFlag f) {
  return (t.flags & static_cast<uint8_t>(f)) != 0;
}

}  // namespace ups
