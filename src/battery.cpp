#include "battery.h"

#include <string.h>

namespace {

struct OcvPoint {
  uint16_t mv;
  uint16_t soc_x10;
};

// Approximate per-cell Li-ion OCV table.  Used only for initialisation and
// slow correction at rest; coulomb counting is the primary source of truth.
const OcvPoint kOcv[] = {
    {3000, 0},   {3300, 50},  {3450, 100}, {3550, 200}, {3650, 300},
    {3700, 400}, {3750, 500}, {3800, 600}, {3900, 700}, {4000, 800},
    {4100, 900}, {4200, 1000},
};

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

void BatteryModel::begin(const Settings& s, uint16_t saved_soc_x10, bool saved_valid) {
  memset(this, 0, sizeof(*this));
  if (saved_valid && saved_soc_x10 <= 1000) {
    soc_x10_ = saved_soc_x10;
    have_soc_ = true;
  } else {
    soc_x10_ = 500;
    have_soc_ = false;
  }
  (void)s;
  updateFlags(s, packNominalMv(s), false, 0);
}

void BatteryModel::setSocX10(uint16_t value) {
  soc_x10_ = value > 1000 ? 1000 : value;
  have_soc_ = true;
  coulomb_rem_ = 0;
}

uint16_t BatteryModel::ocvSocX10(const Settings& s, int32_t vbat_mv) {
  if (s.cells == 0) {
    return 0;
  }
  int32_t cell_mv = vbat_mv / (int32_t)s.cells;
  if (cell_mv <= kOcv[0].mv) {
    return 0;
  }
  const size_t n = sizeof(kOcv) / sizeof(kOcv[0]);
  if (cell_mv >= kOcv[n - 1].mv) {
    return 1000;
  }
  for (size_t i = 1; i < n; ++i) {
    if (cell_mv <= kOcv[i].mv) {
      int32_t dv = kOcv[i].mv - kOcv[i - 1].mv;
      int32_t ds = kOcv[i].soc_x10 - kOcv[i - 1].soc_x10;
      return static_cast<uint16_t>(kOcv[i - 1].soc_x10 +
                                   (cell_mv - kOcv[i - 1].mv) * ds / dv);
    }
  }
  return 1000;
}

void BatteryModel::integrate(const Settings& s, int32_t ibat_ma, uint32_t dt_ms) {
  if (s.capacity_mAh == 0) {
    return;
  }
  // 1 mA*ms == 1 uA*s.  0.1 % of capacity == capacity_mAh * 3600 uA*s.
  coulomb_rem_ += static_cast<int64_t>(ibat_ma) * static_cast<int64_t>(dt_ms);
  const int64_t unit = static_cast<int64_t>(s.capacity_mAh) * 3600;
  if (unit <= 0) {
    return;
  }

  while (coulomb_rem_ >= unit) {
    coulomb_rem_ -= unit;
    if (soc_x10_ < 1000) {
      ++soc_x10_;
    }
    if (soc_x10_ >= 1000) {
      soc_x10_ = 1000;
      coulomb_rem_ = 0;
      break;
    }
  }
  while (coulomb_rem_ <= -unit) {
    coulomb_rem_ += unit;
    if (soc_x10_ > 0) {
      --soc_x10_;
    }
    if (soc_x10_ == 0) {
      coulomb_rem_ = 0;
      break;
    }
  }
}

void BatteryModel::updateFlags(const Settings& s, int32_t vbat_mv, bool discharging,
                               uint32_t dt_ms) {
  // Voltage-based low/critical detection is only allowed while discharging.
  // While charging, the charger raises the terminal voltage, so relying on
  // voltage would produce false low-battery alarms.
  if (discharging) {
    if (vbat_mv <= packLowMv(s)) {
      low_voltage_ms_ += dt_ms;
    } else if (low_voltage_ms_ > 2000) {
      low_voltage_ms_ -= 2000;
    } else {
      low_voltage_ms_ = 0;
    }

    if (vbat_mv <= packCriticalMv(s)) {
      critical_voltage_ms_ += dt_ms;
    } else if (critical_voltage_ms_ > 1000) {
      critical_voltage_ms_ -= 1000;
    } else {
      critical_voltage_ms_ = 0;
    }
  } else {
    // While charging (or idle) the terminal voltage is not a valid low-battery
    // signal.  Clear the timers; otherwise a low-voltage debounce from before
    // the charger started would keep low() asserted even after SoC recovers.
    low_voltage_ms_ = 0;
    critical_voltage_ms_ = 0;
  }

  const uint32_t low_pct = static_cast<uint32_t>(s.low_pct) * 10u;
  const uint32_t crit_pct = static_cast<uint32_t>(s.critical_pct) * 10u;

  low_ = (static_cast<uint32_t>(soc_x10_) <= low_pct) || (low_voltage_ms_ >= 5000);
  critical_ = (static_cast<uint32_t>(soc_x10_) <= crit_pct) ||
              (critical_voltage_ms_ >= 2000);
  full_ = (soc_x10_ >= 995);
}

void BatteryModel::update(const Settings& s,
                          int32_t vbat_mv,
                          int32_t ibat_ma,
                          bool current_valid,
                          bool ac_present,
                          bool charging,
                          bool charge_done,
                          bool discharging,
                          uint32_t dt_ms) {
  if (dt_ms > 10000) {
    dt_ms = 10000;
  }

  if (!have_soc_) {
    soc_x10_ = ocvSocX10(s, vbat_mv);
    have_soc_ = true;
  }

  const int32_t abs_i = ibat_ma < 0 ? -ibat_ma : ibat_ma;

  if (current_valid) {
    integrate(s, ibat_ma, dt_ms);

    // Rest detection: after a minute without meaningful current, nudge the
    // estimate toward the OCV curve.  This removes slow coulomb-counter drift.
    if (abs_i < 100) {
      rest_ms_ += dt_ms;
    } else {
      rest_ms_ = 0;
      ocv_tick_ms_ = 0;
    }
    if (rest_ms_ >= 60000) {
      ocv_tick_ms_ += dt_ms;
      if (ocv_tick_ms_ >= 10000) {
        ocv_tick_ms_ = 0;
        int32_t target = ocvSocX10(s, vbat_mv);
        int32_t diff = target - static_cast<int32_t>(soc_x10_);
        if (diff > 30) {
          soc_x10_ = static_cast<uint16_t>(clamp32(static_cast<int32_t>(soc_x10_) + diff / 8,
                                                   0, 1000));
        } else if (diff < -30) {
          soc_x10_ = static_cast<uint16_t>(clamp32(static_cast<int32_t>(soc_x10_) + diff / 8,
                                                   0, 1000));
        }
      }
    }
  } else {
    rest_ms_ = 0;
    // No current sensor: while discharging, slowly follow OCV so the displayed
    // estimate remains useful.  Do not do this while charging.
    if (discharging && !ac_present) {
      ocv_tick_ms_ += dt_ms;
      if (ocv_tick_ms_ >= 30000) {
        ocv_tick_ms_ = 0;
        int32_t target = ocvSocX10(s, vbat_mv);
        int32_t diff = target - static_cast<int32_t>(soc_x10_);
        if (diff != 0) {
          soc_x10_ = static_cast<uint16_t>(
              clamp32(static_cast<int32_t>(soc_x10_) + (diff > 0 ? 1 : -1), 0, 1000));
        }
      }
    }
  }

  if (charge_done && ac_present) {
    soc_x10_ = 1000;
    coulomb_rem_ = 0;
  }

  // Tail-current full detection for chargers without a DONE pin.
  if (charging && current_valid && ac_present && vbat_mv >= packFullMv(s) - 60) {
    int32_t tail_ma = static_cast<int32_t>(s.max_charge_ma) / 30;
    if (tail_ma < 50) {
      tail_ma = 50;
    }
    if (abs_i < tail_ma) {
      full_hold_ms_ += dt_ms;
    } else {
      full_hold_ms_ = 0;
    }
    if (full_hold_ms_ >= 60000) {
      soc_x10_ = 1000;
      coulomb_rem_ = 0;
    }
  } else {
    full_hold_ms_ = 0;
  }

  if (discharging && vbat_mv <= packEmptyMv(s)) {
    soc_x10_ = 0;
    coulomb_rem_ = 0;
  }

  soc_x10_ = static_cast<uint16_t>(clamp32(soc_x10_, 0, 1000));
  updateFlags(s, vbat_mv, discharging, dt_ms);
}

uint32_t BatteryModel::remaining_mAh(const Settings& s) const {
  return (static_cast<uint32_t>(s.capacity_mAh) * soc_x10_ + 500u) / 1000u;
}

uint32_t BatteryModel::runtimeSeconds(const Settings& s, int32_t load_ma) const {
  if (load_ma <= 0) {
    return 0;
  }
  uint32_t rem = remaining_mAh(s);
  uint64_t seconds = static_cast<uint64_t>(rem) * 3600ull / static_cast<uint32_t>(load_ma);
  // Keep the value sane for telemetry.  Seven days is plenty for a small UPS.
  if (seconds > 604800ull) {
    seconds = 604800ull;
  }
  return static_cast<uint32_t>(seconds);
}
