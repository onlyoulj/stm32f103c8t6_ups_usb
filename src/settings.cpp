#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void setError(char* err, size_t cap, const char* msg) {
  if (err != nullptr && cap > 0) {
    snprintf(err, cap, "%s", msg);
  }
}

bool parseLong(const char* text, long& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  long value = strtol(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

enum FieldType : uint8_t { FIELD_U16, FIELD_I16, FIELD_BOOL };

struct IntKey {
  const char* name;
  size_t offset;
  FieldType type;
};

#define KEY_U16(name, field) {name, offsetof(Settings, field), FIELD_U16}
#define KEY_I16(name, field) {name, offsetof(Settings, field), FIELD_I16}
#define KEY_BOOL(name, field) {name, offsetof(Settings, field), FIELD_BOOL}

const IntKey kIntKeys[] = {
    KEY_U16("cells", cells),
    KEY_U16("capacity_mah", capacity_mAh),
    KEY_U16("low_pct", low_pct),
    KEY_U16("critical_pct", critical_pct),
    KEY_U16("cutoff_grace_s", cutoff_grace_s),
    KEY_U16("ac_restore_s", ac_restore_s),
    KEY_U16("cell_nominal_mv", cell_nominal_mv),
    KEY_U16("cell_full_mv", cell_full_mv),
    KEY_U16("cell_empty_mv", cell_empty_mv),
    KEY_U16("cell_low_mv", cell_low_mv),
    KEY_U16("cell_critical_mv", cell_critical_mv),
    KEY_U16("cell_ov_mv", cell_ov_mv),
    KEY_I16("vbat_scale", vbat_scale_permille),
    KEY_I16("vbat_offset", vbat_offset_mv),
    KEY_I16("vin_scale", vin_scale_permille),
    KEY_I16("vin_offset", vin_offset_mv),
    KEY_I16("vout_scale", vout_scale_permille),
    KEY_I16("vout_offset", vout_offset_mv),
    KEY_U16("shunt_mohm", shunt_mohm),
    KEY_U16("max_charge_ma", max_charge_ma),
    KEY_U16("max_discharge_ma", max_discharge_ma),
    KEY_U16("max_load_ma", max_load_ma),
    KEY_U16("assumed_load_ma", assumed_load_ma),
    KEY_U16("vin_present_mv", vin_present_mv),
    KEY_U16("vin_nominal_mv", vin_nominal_mv),
    KEY_U16("vin_max_mv", vin_max_mv),
    KEY_U16("vout_nominal_mv", vout_nominal_mv),
    KEY_U16("vout_uv_mv", vout_uv_mv),
    KEY_U16("vout_ov_mv", vout_ov_mv),
    KEY_I16("charge_max_temp", charge_max_temp_c_x10),
    KEY_I16("discharge_min_temp", discharge_min_temp_c_x10),
    KEY_I16("discharge_max_temp", discharge_max_temp_c_x10),
    KEY_I16("temp_fault", temp_fault_c_x10),
    KEY_I16("temp_offset", temp_offset_c_x10),
    KEY_U16("nominal_freq_x10", nominal_freq_x10),
    KEY_BOOL("beeper_enabled", beeper_enabled),
    KEY_BOOL("ntc_enabled", ntc_enabled),
    KEY_BOOL("ina_invert", ina_invert),
    KEY_BOOL("auto_recover", auto_recover),
};

#undef KEY_U16
#undef KEY_I16
#undef KEY_BOOL

}  // namespace

void settingsDefaults(Settings& s) {
  memset(&s, 0, sizeof(s));
  s.magic = SETTINGS_MAGIC;
  s.version = SETTINGS_VERSION;

  s.cells = 2;
  s.capacity_mAh = 5000;
  s.low_pct = 20;
  s.critical_pct = 5;
  s.cutoff_grace_s = 180;
  s.ac_restore_s = 3;

  s.cell_nominal_mv = 3700;
  s.cell_full_mv = 4200;
  s.cell_empty_mv = 3000;
  s.cell_low_mv = 3450;
  s.cell_critical_mv = 3250;
  s.cell_ov_mv = 4300;

  s.vbat_scale_permille = 1000;
  s.vbat_offset_mv = 0;
  s.vin_scale_permille = 1000;
  s.vin_offset_mv = 0;
  s.vout_scale_permille = 1000;
  s.vout_offset_mv = 0;

  s.shunt_mohm = 20;
  s.max_charge_ma = 2000;
  s.max_discharge_ma = 6000;
  s.max_load_ma = 2000;
  s.assumed_load_ma = 800;

  s.vin_present_mv = 8000;
  s.vin_nominal_mv = 12000;
  s.vin_max_mv = 16000;
  s.vout_nominal_mv = 12000;
  s.vout_uv_mv = 10500;
  s.vout_ov_mv = 13500;

  s.charge_max_temp_c_x10 = 450;       // 45.0 C
  s.discharge_min_temp_c_x10 = -100;   // -10.0 C
  s.discharge_max_temp_c_x10 = 600;    // 60.0 C
  s.temp_fault_c_x10 = 700;            // 70.0 C
  s.temp_offset_c_x10 = 0;

  s.nominal_freq_x10 = 500;
  s.beeper_enabled = 1;
  s.ntc_enabled = 1;
  s.ina_invert = 0;
  s.auto_recover = 1;

  settingsFinalize(s);
}

bool settingsValidate(const Settings& s, char* err, size_t cap) {
  if (s.cells < 1 || s.cells > SETTINGS_MAX_CELLS) {
    setError(err, cap, "cells out of range");
    return false;
  }
  if (s.capacity_mAh < 500 || s.capacity_mAh > 60000) {
    setError(err, cap, "capacity_mah out of range");
    return false;
  }
  if (s.low_pct > 90 || s.critical_pct >= s.low_pct || s.critical_pct < 1) {
    setError(err, cap, "battery percentages invalid");
    return false;
  }
  if (s.cutoff_grace_s > 3600 || s.ac_restore_s < 1 || s.ac_restore_s > 60) {
    setError(err, cap, "timing out of range");
    return false;
  }

  if (s.cell_nominal_mv < 2500 || s.cell_nominal_mv > 4000 ||
      s.cell_full_mv < 3000 || s.cell_full_mv > 4500 ||
      s.cell_empty_mv < 2000 || s.cell_empty_mv > 3500 ||
      s.cell_low_mv < 2500 || s.cell_low_mv > 4000 ||
      s.cell_critical_mv < 2200 || s.cell_critical_mv > 3900 ||
      s.cell_ov_mv < 3500 || s.cell_ov_mv > 4600) {
    setError(err, cap, "cell voltage out of range");
    return false;
  }
  if (!(s.cell_empty_mv < s.cell_critical_mv &&
        s.cell_critical_mv < s.cell_low_mv &&
        s.cell_low_mv < s.cell_full_mv &&
        s.cell_full_mv < s.cell_ov_mv)) {
    setError(err, cap, "cell voltage order invalid");
    return false;
  }
  if (s.cell_nominal_mv < s.cell_critical_mv || s.cell_nominal_mv > s.cell_full_mv) {
    setError(err, cap, "cell nominal invalid");
    return false;
  }

  if (s.vbat_scale_permille < 500 || s.vbat_scale_permille > 2000 ||
      s.vin_scale_permille < 500 || s.vin_scale_permille > 2000 ||
      s.vout_scale_permille < 500 || s.vout_scale_permille > 2000) {
    setError(err, cap, "voltage scale out of range");
    return false;
  }
  if (s.vbat_offset_mv < -5000 || s.vbat_offset_mv > 5000 ||
      s.vin_offset_mv < -5000 || s.vin_offset_mv > 5000 ||
      s.vout_offset_mv < -5000 || s.vout_offset_mv > 5000) {
    setError(err, cap, "voltage offset out of range");
    return false;
  }

  if (s.shunt_mohm > 1000) {
    setError(err, cap, "shunt_mohm out of range");
    return false;
  }
  if (s.max_charge_ma < 100 || s.max_charge_ma > 20000 ||
      s.max_discharge_ma < 100 || s.max_discharge_ma > 30000 ||
      s.max_load_ma < 100 || s.max_load_ma > 10000 ||
      s.assumed_load_ma > s.max_load_ma) {
    setError(err, cap, "current limits out of range");
    return false;
  }

  if (s.vin_nominal_mv < 3000 || s.vin_nominal_mv > 40000 ||
      s.vin_present_mv < 3000 || s.vin_present_mv >= s.vin_nominal_mv ||
      s.vin_max_mv <= s.vin_nominal_mv || s.vin_max_mv > 60000) {
    setError(err, cap, "input rail thresholds invalid");
    return false;
  }
  if (s.vout_nominal_mv < 3000 || s.vout_nominal_mv > 30000 ||
      s.vout_uv_mv < 2000 || s.vout_uv_mv >= s.vout_nominal_mv ||
      s.vout_ov_mv <= s.vout_nominal_mv || s.vout_ov_mv > 35000) {
    setError(err, cap, "output rail thresholds invalid");
    return false;
  }

  if (s.charge_max_temp_c_x10 < 0 || s.charge_max_temp_c_x10 > 600 ||
      s.discharge_min_temp_c_x10 < -300 || s.discharge_min_temp_c_x10 > 200 ||
      s.discharge_max_temp_c_x10 < 0 || s.discharge_max_temp_c_x10 > 800 ||
      s.discharge_min_temp_c_x10 >= s.discharge_max_temp_c_x10 ||
      s.temp_fault_c_x10 <= s.discharge_max_temp_c_x10 ||
      s.temp_fault_c_x10 > 1000 ||
      s.charge_max_temp_c_x10 >= s.temp_fault_c_x10 ||
      s.temp_offset_c_x10 < -200 || s.temp_offset_c_x10 > 200) {
    setError(err, cap, "temperature thresholds invalid");
    return false;
  }

  if (s.nominal_freq_x10 < 450 || s.nominal_freq_x10 > 650) {
    setError(err, cap, "nominal frequency invalid");
    return false;
  }
  if (s.beeper_enabled > 1 || s.ntc_enabled > 1 ||
      s.ina_invert > 1 || s.auto_recover > 1) {
    setError(err, cap, "boolean value invalid");
    return false;
  }
  return true;
}

bool settingsApply(Settings& s, const char* key, const char* value, char* err, size_t cap) {
  if (key == nullptr || value == nullptr) {
    setError(err, cap, "missing key or value");
    return false;
  }
  for (const IntKey& k : kIntKeys) {
    if (strcmp(key, k.name) != 0) {
      continue;
    }
    long v = 0;
    if (!parseLong(value, v)) {
      setError(err, cap, "not an integer");
      return false;
    }
    if (k.type == FIELD_U16) {
      if (v < 0 || v > 65535) {
        setError(err, cap, "value out of range");
        return false;
      }
      *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(&s) + k.offset) =
          static_cast<uint16_t>(v);
    } else if (k.type == FIELD_I16) {
      if (v < -32768 || v > 32767) {
        setError(err, cap, "value out of range");
        return false;
      }
      *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(&s) + k.offset) =
          static_cast<int16_t>(v);
    } else {
      if (v < 0 || v > 1) {
        setError(err, cap, "boolean must be 0 or 1");
        return false;
      }
      *(reinterpret_cast<uint8_t*>(&s) + k.offset) = static_cast<uint8_t>(v);
    }
    settingsFinalize(s);
    return true;
  }
  setError(err, cap, "unknown key");
  return false;
}

uint32_t settingsCrc(const Settings& s) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&s);
  const size_t len = offsetof(Settings, crc);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
  }
  return ~crc;
}

void settingsFinalize(Settings& s) {
  s.magic = SETTINGS_MAGIC;
  s.version = SETTINGS_VERSION;
  s.crc = settingsCrc(s);
}
