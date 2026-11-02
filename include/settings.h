#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Persistent and runtime configuration.
 *
 * All voltages are millivolts, currents milliamps, temperatures 0.1 degC,
 * time seconds, percentages whole percent, SoC 0.1 percent.
 *
 * The struct is stored in EEPROM as-is.  Bump SETTINGS_VERSION whenever the
 * layout changes so old blobs are discarded instead of misinterpreted.
 */

#define UPS_FW_VERSION "1.0.0"
#define UPS_PROTO_VERSION 1
#define UPS_DEVICE_NAME "STM32-DC-UPS"
#define UPS_MANUFACTURER "XiaoBuUPS"

constexpr uint32_t SETTINGS_MAGIC   = 0x55505331u;  // "UPS1"
constexpr uint16_t SETTINGS_VERSION = 2;
constexpr uint16_t SETTINGS_MAX_CELLS = 6;
constexpr int16_t  TEMP_INVALID_C_X10 = 9999;

struct Settings {
  uint32_t magic;
  uint16_t version;

  // Battery ---------------------------------------------------------------
  uint16_t cells;              // series cell count (1..6)
  uint16_t capacity_mAh;       // pack capacity (parallel cells summed)
  uint16_t low_pct;            // low-battery warning, percent
  uint16_t critical_pct;       // critical-battery threshold, percent
  uint16_t cutoff_grace_s;     // delay between critical and output cut
  uint16_t ac_restore_s;       // AC must be stable this long before transfer

  // Per-cell voltage thresholds (Li-ion defaults; LiFePO4 users must change)
  uint16_t cell_nominal_mv;    // 3700 for Li-ion
  uint16_t cell_full_mv;       // 4200
  uint16_t cell_empty_mv;      // 3000
  uint16_t cell_low_mv;        // 3450
  uint16_t cell_critical_mv;   // 3250
  uint16_t cell_ov_mv;         // 4300

  // Voltage calibration (applied after the divider) ----------------------
  int16_t  vbat_scale_permille;  // 1000 = nominal
  int16_t  vbat_offset_mv;
  int16_t  vin_scale_permille;
  int16_t  vin_offset_mv;
  int16_t  vout_scale_permille;
  int16_t  vout_offset_mv;

  // Current / load --------------------------------------------------------
  uint16_t shunt_mohm;          // battery INA219 shunt; 0 disables current
  uint16_t max_charge_ma;
  uint16_t max_discharge_ma;
  uint16_t max_load_ma;         // output-side nominal full-scale for load %
  uint16_t assumed_load_ma;     // used for runtime when no output sensor

  // Input / output rails --------------------------------------------------
  uint16_t vin_present_mv;      // below this the input is considered failed
  uint16_t vin_nominal_mv;
  uint16_t vin_max_mv;          // above this: input over-voltage
  uint16_t vout_nominal_mv;
  uint16_t vout_uv_mv;
  uint16_t vout_ov_mv;

  // Thermal ---------------------------------------------------------------
  int16_t  charge_max_temp_c_x10;
  int16_t  discharge_min_temp_c_x10;
  int16_t  discharge_max_temp_c_x10;
  int16_t  temp_fault_c_x10;
  int16_t  temp_offset_c_x10;

  // Miscellaneous ---------------------------------------------------------
  uint16_t nominal_freq_x10;    // reported via Megatec; DC input has no mains
  uint8_t  beeper_enabled;
  uint8_t  ntc_enabled;
  uint8_t  ina_invert;          // 1 if positive current means discharge
  uint8_t  auto_recover;

  uint8_t  reserved[4];
  uint32_t crc;
};

// Pack-level derived thresholds ------------------------------------------------
inline int32_t packNominalMv(const Settings& s) { return (int32_t)s.cells * s.cell_nominal_mv; }
inline int32_t packFullMv(const Settings& s)    { return (int32_t)s.cells * s.cell_full_mv; }
inline int32_t packEmptyMv(const Settings& s)   { return (int32_t)s.cells * s.cell_empty_mv; }
inline int32_t packLowMv(const Settings& s)     { return (int32_t)s.cells * s.cell_low_mv; }
inline int32_t packCriticalMv(const Settings& s){ return (int32_t)s.cells * s.cell_critical_mv; }
inline int32_t packOvMv(const Settings& s)      { return (int32_t)s.cells * s.cell_ov_mv; }
inline int32_t packPresentMv(const Settings& s) { return (int32_t)s.cells * 1500; }

void settingsDefaults(Settings& s);

// Returns false and fills err on invalid ranges / cross-field relations.
bool settingsValidate(const Settings& s, char* err, size_t cap);

// Apply "key=value" to a candidate struct.  Validation is done by the caller
// on a copy so a rejected value never changes live configuration.
// Keys are lower-case; values are decimal integers or 0/1 for booleans.
bool settingsApply(Settings& s, const char* key, const char* value, char* err, size_t cap);

uint32_t settingsCrc(const Settings& s);
void settingsFinalize(Settings& s);
