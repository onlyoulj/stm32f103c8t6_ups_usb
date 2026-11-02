#include "app.h"

#include <USBSerial.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"

namespace {
constexpr uint32_t CONTROL_PERIOD_MS = 50;
constexpr uint32_t BOOT_GRACE_MS = 800;
constexpr uint32_t AC_FAIL_DEBOUNCE_PIN_MS = 30;
constexpr uint32_t AC_FAIL_DEBOUNCE_ADC_MS = 100;
constexpr uint32_t OUTPUT_ON_GRACE_MS = 1500;
constexpr uint32_t OUTPUT_TRANSFER_GRACE_MS = 2000;
constexpr uint32_t OC_TRIP_MS = 1000;
constexpr uint32_t OUTPUT_TRIP_MS = 500;
constexpr uint32_t FAULT_RETRY_OC_MS = 30000;
constexpr uint32_t FAULT_RETRY_OUTPUT_MS = 5000;
constexpr uint32_t HEALTHY_RETRY_RESET_MS = 10ul * 60ul * 1000ul;
constexpr uint8_t MAX_FAULT_RETRIES = 3;
constexpr uint32_t CHARGE_OC_BLOCK_MS = 30000;
constexpr uint32_t CHARGER_STALE_MS = 10000;
constexpr uint32_t RESTORE_AFTER_AC_MS = 10000;
constexpr uint32_t STATE_SAVE_INTERVAL_MS = 30ul * 60ul * 1000ul;
constexpr uint32_t STATE_SAVE_DELAY_MS = 5000;
constexpr uint32_t STATE_SAVE_RETRY_MS = 5ul * 60ul * 1000ul;
constexpr uint32_t TEST_BOOST_PRE_MS = 150;
constexpr uint32_t TEST_CUT_MS = 100;
constexpr uint32_t TEST_RESTORE_MS = 500;
constexpr uint32_t TEST_MAX_UNTIL_LOW_MS = 60ul * 60ul * 1000ul;

int32_t iabs32(int32_t v) {
  return v < 0 ? -v : v;
}

void appendError(char* err, size_t cap, const char* text) {
  if (err != nullptr && cap > 0) {
    snprintf(err, cap, "%s", text);
  }
}
}  // namespace

App::App()
    : store_(),
      sensors_(),
      battery_(),
      ui_(),
      comm_(SerialUSB, *this),
      state_(ups::State::Boot),
      ac_present_(false),
      ac_raw_last_(false),
      ac_raw_change_ms_(0),
      boot_ms_(0),
      last_control_ms_(0),
      last_debug_ms_(0),
      last_second_s_(0),
      last_save_ms_(0),
      next_state_save_ms_(0),
      uptime_s_(0),
      total_runtime_s_(0),
      ac_fail_count_(0),
      cycle_count_(0),
      output_on_(false),
      boost_on_(false),
      charge_on_(false),
      fan_on_(false),
      ac_cut_on_(false),
      charging_(false),
      charge_done_(false),
      battery_present_(false),
      battery_ov_(false),
      charge_temp_ok_(false),
      vin_fault_mv_(0),
      last_good_vin_mv_(0),
      iout_ma_(0),
      load_ma_(0),
      load_pct_(0),
      runtime_s_(0),
      faults_(0),
      warnings_(0),
      latched_faults_(0),
      fault_retry_ms_(0),
      last_fault_ms_(0),
      fault_retry_count_(0),
      overcurrent_ms_(0),
      output_bad_ms_(0),
      output_on_ms_(0),
      output_check_grace_until_(0),
      charge_oc_block_until_(0),
      charger_stale_ms_(0),
      boost_off_at_ms_(0),
      nvm_save_failed_(false),
      shutdown_reason_(ShutdownReason::None),
      shutdown_at_ms_(0),
      shutdown_restore_s_(0),
      output_cut_(false),
      restore_wait_ac_(false),
      restore_deadline_ms_(0),
      restore_not_before_ms_(0),
      test_phase_(TestPhase::Idle),
      test_phase_ms_(0),
      test_deadline_ms_(0),
      test_active_(false),
      test_until_low_(false),
      test_abort_(false) {}

void App::begin() {
  // Keep the load enabled while the controller boots; the hardware should
  // also have a pull-up on OUT_EN.  Everything else starts disabled.
  setPin(brd::PIN_OUT_EN, true, UPS_OUT_EN_ACTIVE_HIGH != 0);
  pinMode(brd::PIN_OUT_EN, OUTPUT);
  // Leave BOOST_EN high-Z during boot: a hardware pull-up (recommended) keeps
  // the boost running through an MCU reset while the UPS is on battery.
  pinMode(brd::PIN_BOOST_EN, INPUT);
  setPin(brd::PIN_CHG_EN, false, UPS_CHG_EN_ACTIVE_HIGH != 0);
  pinMode(brd::PIN_CHG_EN, OUTPUT);
  setPin(brd::PIN_FAN_EN, false, true);
  pinMode(brd::PIN_FAN_EN, OUTPUT);
  setPin(brd::PIN_AC_CUT_EN, false, UPS_AC_CUT_ACTIVE_HIGH != 0);
  pinMode(brd::PIN_AC_CUT_EN, OUTPUT);
  setPin(brd::PIN_BUZZER, false, UPS_BUZZER_ACTIVE_HIGH != 0);
  pinMode(brd::PIN_BUZZER, OUTPUT);

#if UPS_HAS_AC_OK_PIN
  pinMode(brd::PIN_AC_OK, (UPS_AC_OK_ACTIVE_HIGH != 0) ? INPUT_PULLDOWN : INPUT_PULLUP);
#endif
#if UPS_HAS_CHG_STAT
  pinMode(brd::PIN_CHG_STAT, INPUT_PULLUP);
#endif
#if UPS_HAS_CHG_DONE
  pinMode(brd::PIN_CHG_DONE, INPUT_PULLUP);
#endif

  SerialUSB.begin(115200);
#if UPS_DEBUG_UART
  Serial1.begin(UPS_DEBUG_BAUD);
#endif

  store_.begin();
  const Settings& s = store_.settings();
  const RuntimeState& st = store_.state();
  ac_fail_count_ = st.ac_fail_count;
  cycle_count_ = st.cycle_count;
  total_runtime_s_ = st.total_runtime_s;

  battery_.begin(s, st.soc_x10, store_.hadValidPage());
  sensors_.begin(s);
  ui_.begin();

  boot_ms_ = millis();
  last_control_ms_ = boot_ms_;
  last_debug_ms_ = boot_ms_;
  last_save_ms_ = boot_ms_;
  last_second_s_ = boot_ms_ / 1000u;
  ac_raw_change_ms_ = boot_ms_;
  next_state_save_ms_ = 0;

  sensors_.poll(s, boot_ms_);
  const SensorSample& m = sensors_.sample();
  ac_present_ = readAcRaw(m);
  ac_raw_last_ = ac_present_;
  vin_fault_mv_ = m.vin_mv;
  last_good_vin_mv_ = m.vin_mv;
  battery_present_ = (m.vbat_mv >= packPresentMv(s)) && (m.vbat_ok || m.vbat_from_ina);
  battery_ov_ = m.vbat_mv > packOvMv(s);

  // Start with the output enabled; control() will disable it if no source or
  // a fault is present.  On battery, pre-enable boost to minimise the gap.
  output_on_ = true;
  output_on_ms_ = boot_ms_;
  output_check_grace_until_ = boot_ms_ + OUTPUT_ON_GRACE_MS * 2;
  setPin(brd::PIN_OUT_EN, true, UPS_OUT_EN_ACTIVE_HIGH != 0);
  boost_on_ = !ac_present_;
  pinMode(brd::PIN_BOOST_EN, OUTPUT);
  setPin(brd::PIN_BOOST_EN, boost_on_, UPS_BOOST_EN_ACTIVE_HIGH != 0);
  charge_on_ = false;
  setPin(brd::PIN_CHG_EN, false, UPS_CHG_EN_ACTIVE_HIGH != 0);

  state_ = ups::State::Boot;
  watchdogBegin();
}

void App::loop() {
  const uint32_t now = millis();
  const Settings& s = store_.settings();

  sensors_.poll(s, now);
  updateAc(now);

  const uint32_t elapsed = now - last_control_ms_;
  if (elapsed >= CONTROL_PERIOD_MS) {
    const uint32_t dt = elapsed > 1000u ? 1000u : elapsed;
    last_control_ms_ = now;
    control(now, dt);
  }

  comm_.poll();

  UiInput ui_in;
  buildUiInput(ui_in);
  ui_.poll(now, ui_in);
  if (ui_.consumeShortPress()) {
    commToggleBeeper();
  }
  if (ui_.consumeLongPress()) {
    clearLatchedFaults();
    shutdown_at_ms_ = 0;
    shutdown_restore_s_ = 0;
    shutdown_reason_ = ShutdownReason::None;
    output_cut_ = false;
    restore_deadline_ms_ = 0;
    restore_not_before_ms_ = 0;
    restore_wait_ac_ = false;
    stopTest();
    output_check_grace_until_ = now + OUTPUT_ON_GRACE_MS;
    requestStateSave(now);
  }

  updateRuntime(now);

  const bool was_dirty = store_.dirty();
  store_.tick(now);
  if (was_dirty && !store_.dirty()) {
    last_save_ms_ = now;
    next_state_save_ms_ = 0;
    nvm_save_failed_ = false;
  }

  saveStateIfDue(now);
  debugPrint(now);
  watchdogFeed();
}

void App::updateAc(uint32_t now) {
  const Settings& s = store_.settings();
  const SensorSample& m = sensors_.sample();
  const bool raw = readAcRaw(m);

  if (raw != ac_raw_last_) {
    ac_raw_last_ = raw;
    ac_raw_change_ms_ = now;
  }

  const uint32_t debounce_ms = raw
      ? (static_cast<uint32_t>(s.ac_restore_s) * 1000u)
      : (UPS_HAS_AC_OK_PIN ? AC_FAIL_DEBOUNCE_PIN_MS : AC_FAIL_DEBOUNCE_ADC_MS);

  if (raw != ac_present_ && (now - ac_raw_change_ms_) >= debounce_ms) {
    ac_present_ = raw;
    requestStateSave(now);

    if (!raw) {
      ++ac_fail_count_;
      ++cycle_count_;
      store_.stateMutable().ac_fail_count = ac_fail_count_;
      store_.stateMutable().cycle_count = cycle_count_;
      vin_fault_mv_ = last_good_vin_mv_;
      output_check_grace_until_ = now + OUTPUT_TRANSFER_GRACE_MS;

      if (output_on_) {
        boost_on_ = true;
        setPin(brd::PIN_BOOST_EN, true, UPS_BOOST_EN_ACTIVE_HIGH != 0);
      }
      charge_on_ = false;
      setPin(brd::PIN_CHG_EN, false, UPS_CHG_EN_ACTIVE_HIGH != 0);

      if (test_phase_ != TestPhase::Idle) {
        stopTest();
      }
    } else {
      vin_fault_mv_ = m.vin_mv;
      last_good_vin_mv_ = m.vin_mv;
      boost_off_at_ms_ = now + OUTPUT_TRANSFER_GRACE_MS;
      output_check_grace_until_ = now + OUTPUT_TRANSFER_GRACE_MS;

      if (shutdown_reason_ == ShutdownReason::LowBattery && !output_cut_) {
        shutdown_at_ms_ = 0;
        shutdown_reason_ = ShutdownReason::None;
      }
      if (output_cut_ && restore_wait_ac_) {
        restore_wait_ac_ = false;
        uint32_t deadline = now + RESTORE_AFTER_AC_MS;
        if (restore_not_before_ms_ > deadline) {
          deadline = restore_not_before_ms_;
        }
        restore_deadline_ms_ = deadline;
      }
      if (test_phase_ != TestPhase::Idle) {
        stopTest();
      }
    }
  }

  if (ac_present_ && m.vin_mv >= s.vin_present_mv) {
    last_good_vin_mv_ = m.vin_mv;
    vin_fault_mv_ = m.vin_mv;
  }
}

void App::control(uint32_t now, uint32_t dt_ms) {
  const Settings& s = store_.settings();
  const SensorSample& m = sensors_.sample();

  // Battery presence and over-voltage hysteresis.
  battery_present_ = (m.vbat_mv >= packPresentMv(s)) && (m.vbat_ok || m.vbat_from_ina);
  if (m.vbat_mv > packOvMv(s)) {
    battery_ov_ = true;
  } else if (m.vbat_mv < packOvMv(s) - 150) {
    battery_ov_ = false;
  }

  charge_temp_ok_ = !s.ntc_enabled ||
                    (m.ntc_ok && m.temp_c_x10 >= 0 &&
                     m.temp_c_x10 <= s.charge_max_temp_c_x10);

  const bool chg_pin = chargeStatusActive();
  const bool done_pin = chargeDoneActive();
  const bool pin_charging = UPS_HAS_CHG_STAT && chg_pin;
  const bool pin_done = UPS_HAS_CHG_DONE && done_pin;

  charging_ = ac_present_ && charge_on_ &&
              (pin_charging ||
               (!UPS_HAS_CHG_STAT && m.ina_ok && m.ibat_ma > 50));
  charge_done_ = ac_present_ && charge_on_ &&
                 (pin_done || battery_.full());
  if (!ac_present_) {
    charging_ = false;
    charge_done_ = false;
  }

  const bool discharging = !ac_present_ && output_on_ && battery_present_;
  battery_.update(s, m.vbat_mv, m.ibat_ma, m.ina_ok, ac_present_,
                  charging_, charge_done_, discharging, dt_ms);

  // Output current, load percentage and runtime.
  int32_t iout = 0;
  if (output_on_) {
    if (m.iout_ok) {
      iout = iabs32(m.iout_ma);
    } else if (!ac_present_ && m.ina_ok && m.vout_mv > 500) {
      iout = static_cast<int32_t>(
          static_cast<int64_t>(iabs32(m.ibat_ma)) * m.vbat_mv * 9 /
          (10 * static_cast<int64_t>(m.vout_mv)));
    } else {
      iout = static_cast<int32_t>(s.assumed_load_ma);
    }
  }
  iout_ma_ = iout;
  if (s.max_load_ma > 0) {
    load_pct_ = static_cast<uint16_t>(
        clamp32(static_cast<int32_t>(static_cast<int64_t>(iout) * 100 / s.max_load_ma),
                0, 100));
  } else {
    load_pct_ = 0;
  }

  int32_t battery_load_ma = 0;
  if (output_on_ && battery_present_) {
    if (!ac_present_ && m.ina_ok) {
      battery_load_ma = iabs32(m.ibat_ma);
    } else if (m.vbat_mv > 500) {
      const int64_t out_mw = static_cast<int64_t>(iout) * m.vout_mv / 1000;
      battery_load_ma = static_cast<int32_t>(
          out_mw * 1000 / m.vbat_mv * 100 / 90);
    }
    if (battery_load_ma < 0) {
      battery_load_ma = 0;
    }
    runtime_s_ = battery_.runtimeSeconds(s, battery_load_ma);
  } else {
    runtime_s_ = 0;
  }
  load_ma_ = battery_load_ma;

  updateProtections(now, dt_ms);
  updateTest(now);
  updateShutdown(now);
  updateOutputs(now);
  updateState(now);

  RuntimeState& rst = store_.stateMutable();
  rst.soc_x10 = battery_.socX10();
  rst.ac_fail_count = ac_fail_count_;
  rst.cycle_count = cycle_count_;
  rst.total_runtime_s = total_runtime_s_;
  if (faults_ != 0) {
    rst.last_fault = faults_;
  }
}

void App::updateProtections(uint32_t now, uint32_t dt_ms) {
  faults_ = 0;
  warnings_ = 0;
  const Settings& s = store_.settings();
  const SensorSample& m = sensors_.sample();

  if (!store_.settingsValid()) {
    warnings_ |= ups::WARN_CONFIG_INVALID;
  }
  if (nvm_save_failed_) {
    warnings_ |= ups::WARN_NVM_ERROR;
  }

  if (now - boot_ms_ < BOOT_GRACE_MS) {
    return;
  }

  if (!battery_present_) {
    warnings_ |= ups::WARN_BATTERY_MISSING;
  }
  if (!m.vbat_ok && !m.vbat_from_ina) {
    warnings_ |= ups::WARN_BATTERY_SENSOR;
  }
  if (s.shunt_mohm > 0 && !m.ina_ok) {
    warnings_ |= ups::WARN_CURRENT_SENSOR;
  }
  if (s.ntc_enabled && !m.ntc_ok) {
    warnings_ |= ups::WARN_TEMP_SENSOR;
  }
  if (battery_ov_) {
    warnings_ |= ups::WARN_BATTERY_OV;
  }
#if UPS_HAS_OUTPUT_INA219
  if (!m.iout_ok) {
    warnings_ |= ups::WARN_OUTPUT_SENSOR;
  }
#endif

  uint16_t computed = 0;
  if (s.ntc_enabled && m.ntc_ok) {
    if (m.temp_c_x10 > s.discharge_max_temp_c_x10 ||
        m.temp_c_x10 < s.discharge_min_temp_c_x10) {
      warnings_ |= ups::WARN_TEMP_DISCHARGE;
    }
    if (m.temp_c_x10 > s.charge_max_temp_c_x10 || m.temp_c_x10 < 0) {
      warnings_ |= ups::WARN_TEMP_CHARGE;
    }
    if (m.temp_c_x10 > s.temp_fault_c_x10) {
      computed |= ups::FAULT_OVER_TEMP;
    }
  }

  if (!battery_present_ && !ac_present_) {
    computed |= ups::FAULT_BATTERY_MISSING;
  }
  if (ac_present_ && (!m.vin_ok || m.vin_mv > s.vin_max_mv)) {
    // !vin_ok means the ADC is at full scale, i.e. the divider is saturated:
    // treat that as over-voltage rather than silently reporting a clamped value.
    computed |= ups::FAULT_INPUT_OV;
  }

  // Discharge / load over-current.
  bool oc = false;
  if (output_on_) {
    if (!ac_present_ && m.ina_ok && (-m.ibat_ma) > static_cast<int32_t>(s.max_discharge_ma)) {
      oc = true;
    }
    if (m.iout_ok && iabs32(m.iout_ma) > static_cast<int32_t>(s.max_load_ma) * 13 / 10) {
      oc = true;
    }
  }
  if (oc) {
    overcurrent_ms_ += dt_ms;
    if (overcurrent_ms_ >= OC_TRIP_MS) {
      latchFault(ups::FAULT_OVER_CURRENT, now, FAULT_RETRY_OC_MS);
      overcurrent_ms_ = 0;
    }
  } else if (overcurrent_ms_ > 2000) {
    overcurrent_ms_ -= 2000;
  } else {
    overcurrent_ms_ = 0;
  }

  // Charge over-current is a charger issue, not a reason to drop the load.
  if (ac_present_ && battery_present_ && m.ina_ok &&
      m.ibat_ma > static_cast<int32_t>(s.max_charge_ma) * 13 / 10) {
    warnings_ |= ups::WARN_CHARGE_OVERCURRENT;
    charge_oc_block_until_ = now + CHARGE_OC_BLOCK_MS;
  }

  // Output voltage window.
  if (output_on_ && static_cast<int32_t>(now - output_check_grace_until_) >= 0) {
    const bool adc_saturated = !m.vout_ok;
    const bool bad = adc_saturated ||
                     (m.vout_mv < s.vout_uv_mv) ||
                     (m.vout_mv > s.vout_ov_mv);
    if (bad) {
      output_bad_ms_ += dt_ms;
    } else if (output_bad_ms_ > 2000) {
      output_bad_ms_ -= 2000;
    } else {
      output_bad_ms_ = 0;
    }
    if (output_bad_ms_ >= OUTPUT_TRIP_MS) {
      if (adc_saturated || m.vout_mv > s.vout_ov_mv) {
        latchFault(ups::FAULT_OUTPUT_OV, now, FAULT_RETRY_OUTPUT_MS);
      } else {
        latchFault(ups::FAULT_OUTPUT_UV, now, FAULT_RETRY_OUTPUT_MS);
      }
      output_bad_ms_ = 0;
    }
  } else {
    output_bad_ms_ = 0;
  }

  // Charger not reporting charge/full while it should be charging.
  if (chargerStatusWired() && ac_present_ && charge_on_ && battery_present_ &&
      !charging_ && !charge_done_ && battery_.socX10() < 990) {
    charger_stale_ms_ += dt_ms;
    if (charger_stale_ms_ >= CHARGER_STALE_MS) {
      warnings_ |= ups::WARN_CHARGER_STALE;
    }
  } else if (charger_stale_ms_ > 2000) {
    charger_stale_ms_ -= 2000;
  } else {
    charger_stale_ms_ = 0;
  }

  if (s.auto_recover && latched_faults_ != 0 && fault_retry_ms_ != 0 &&
      static_cast<int32_t>(now - fault_retry_ms_) >= 0) {
    latched_faults_ = 0;
    fault_retry_ms_ = 0;
  }

  faults_ = static_cast<uint16_t>(computed | latched_faults_);
  if (faults_ != 0) {
    last_fault_ms_ = now;
  } else if (now - last_fault_ms_ >= HEALTHY_RETRY_RESET_MS) {
    fault_retry_count_ = 0;
  }
}

void App::updateShutdown(uint32_t now) {
  const Settings& s = store_.settings();
  const SensorSample& m = sensors_.sample();
  const bool on_battery = !ac_present_ && output_on_ && battery_present_;

  if (!ac_present_) {
    if (on_battery && battery_.critical()) {
      uint32_t deadline = now + static_cast<uint32_t>(s.cutoff_grace_s) * 1000u;
      if (m.vbat_mv <= packEmptyMv(s)) {
        deadline = now;
      }
      if (!output_cut_ && shutdown_at_ms_ == 0) {
        shutdown_reason_ = ShutdownReason::LowBattery;
        shutdown_at_ms_ = deadline;
        requestStateSave(now);
      } else if (!output_cut_ && static_cast<int32_t>(deadline - shutdown_at_ms_) < 0) {
        shutdown_at_ms_ = deadline;
        if (shutdown_reason_ != ShutdownReason::Host) {
          shutdown_reason_ = ShutdownReason::LowBattery;
        }
      }
    }
  } else if (shutdown_reason_ == ShutdownReason::LowBattery && !output_cut_) {
    shutdown_at_ms_ = 0;
    shutdown_reason_ = ShutdownReason::None;
  }

  if (shutdown_at_ms_ != 0 &&
      static_cast<int32_t>(now - shutdown_at_ms_) >= 0) {
    output_cut_ = true;
    shutdown_at_ms_ = 0;
    requestStateSave(now);

    if (shutdown_reason_ == ShutdownReason::Host) {
      if (shutdown_restore_s_ > 0) {
        restore_not_before_ms_ = now + shutdown_restore_s_ * 1000u;
        if (ac_present_) {
          restore_deadline_ms_ = restore_not_before_ms_;
        } else {
          restore_wait_ac_ = true;
        }
      } else {
        restore_not_before_ms_ = 0;
      }
    } else {
      restore_not_before_ms_ = 0;
      if (ac_present_) {
        restore_deadline_ms_ = now + RESTORE_AFTER_AC_MS;
      } else {
        restore_wait_ac_ = true;
      }
    }
  }

  if (output_cut_ && restore_wait_ac_ && ac_present_) {
    restore_wait_ac_ = false;
    uint32_t deadline = now + RESTORE_AFTER_AC_MS;
    if (restore_not_before_ms_ > deadline) {
      deadline = restore_not_before_ms_;
    }
    restore_deadline_ms_ = deadline;
    requestStateSave(now);
  }
  const bool wait_for_ac = (shutdown_reason_ == ShutdownReason::Host &&
                            shutdown_restore_s_ > 0) ||
                           (shutdown_reason_ == ShutdownReason::LowBattery);
  if (!ac_present_ && restore_deadline_ms_ != 0 && wait_for_ac) {
    // A scheduled shutdown+restore waits for utility to return, as does a
    // low-battery cutoff.  A plain "cancel shutdown" (C) may restore from
    // battery after its 10 s hold.
    restore_deadline_ms_ = 0;
    restore_wait_ac_ = true;
  }
  if (output_cut_ && restore_deadline_ms_ != 0 &&
      static_cast<int32_t>(now - restore_deadline_ms_) >= 0) {
    output_cut_ = false;
    restore_deadline_ms_ = 0;
    restore_not_before_ms_ = 0;
    shutdown_reason_ = ShutdownReason::None;
    shutdown_restore_s_ = 0;
    output_check_grace_until_ = now + OUTPUT_ON_GRACE_MS;
    requestStateSave(now);
  }
}

void App::updateTest(uint32_t now) {
#if UPS_HAS_AC_CUT
  switch (test_phase_) {
    case TestPhase::Idle:
      ac_cut_on_ = false;
      break;
    case TestPhase::BoostPre:
      if (static_cast<int32_t>(now - test_phase_ms_) >= static_cast<int32_t>(TEST_BOOST_PRE_MS)) {
        test_phase_ = TestPhase::CutAc;
        test_phase_ms_ = now;
      }
      break;
    case TestPhase::CutAc:
      ac_cut_on_ = true;
      if (static_cast<int32_t>(now - test_phase_ms_) >= static_cast<int32_t>(TEST_CUT_MS)) {
        test_phase_ = TestPhase::Running;
        test_phase_ms_ = now;
        test_active_ = true;
      }
      break;
    case TestPhase::Running:
      ac_cut_on_ = true;
      if (test_abort_ || output_cut_ || faults_ != 0 ||
          static_cast<int32_t>(now - test_deadline_ms_) >= 0 ||
          battery_.critical() || (test_until_low_ && battery_.low())) {
        test_phase_ = TestPhase::RestoreAc;
        test_phase_ms_ = now;
        test_active_ = false;
      }
      break;
    case TestPhase::RestoreAc:
      ac_cut_on_ = false;
      if (static_cast<int32_t>(now - test_phase_ms_) >= static_cast<int32_t>(TEST_RESTORE_MS)) {
        test_phase_ = TestPhase::Idle;
        test_active_ = false;
        test_abort_ = false;
      }
      break;
  }
#else
  (void)now;
  test_phase_ = TestPhase::Idle;
  test_active_ = false;
  ac_cut_on_ = false;
#endif
}

void App::updateOutputs(uint32_t now) {
  const Settings& s = store_.settings();
  const SensorSample& m = sensors_.sample();

  const bool blocked = faults_ != 0;
  const bool can_output = !output_cut_ && !blocked &&
                          (ac_present_ || battery_present_);
  if (can_output != output_on_) {
    output_on_ = can_output;
    if (output_on_) {
      output_on_ms_ = now;
      output_check_grace_until_ = now + OUTPUT_ON_GRACE_MS;
    } else {
      output_bad_ms_ = 0;
    }
  }
  setPin(brd::PIN_OUT_EN, output_on_, UPS_OUT_EN_ACTIVE_HIGH != 0);

  const bool keep_boost_for_transfer =
      static_cast<int32_t>(now - boost_off_at_ms_) < 0;
  const bool want_boost = output_on_ &&
      (!ac_present_ || test_phase_ != TestPhase::Idle || keep_boost_for_transfer);
  boost_on_ = want_boost;
  setPin(brd::PIN_BOOST_EN, boost_on_, UPS_BOOST_EN_ACTIVE_HIGH != 0);

  const bool want_charge = ac_present_ && battery_present_ && !battery_ov_ &&
      !blocked && test_phase_ == TestPhase::Idle &&
      charge_temp_ok_ && static_cast<int32_t>(now - charge_oc_block_until_) >= 0;
  charge_on_ = want_charge;
  setPin(brd::PIN_CHG_EN, charge_on_, UPS_CHG_EN_ACTIVE_HIGH != 0);

  bool want_fan = false;
#if UPS_HAS_FAN
  const bool hot = s.ntc_enabled && m.ntc_ok && m.temp_c_x10 > 450;
  want_fan = output_on_ && (hot || load_pct_ > 50);
#else
  (void)m;
  (void)s;
#endif
  fan_on_ = want_fan;
  setPin(brd::PIN_FAN_EN, fan_on_, true);

#if UPS_HAS_AC_CUT
  setPin(brd::PIN_AC_CUT_EN, ac_cut_on_, UPS_AC_CUT_ACTIVE_HIGH != 0);
#else
  ac_cut_on_ = false;
  setPin(brd::PIN_AC_CUT_EN, false, UPS_AC_CUT_ACTIVE_HIGH != 0);
#endif
}

void App::updateState(uint32_t now) {
  if (now - boot_ms_ < BOOT_GRACE_MS) {
    state_ = ups::State::Boot;
    return;
  }
  if (faults_ != 0) {
    state_ = ups::State::Fault;
  } else if (output_cut_) {
    state_ = ups::State::Shutdown;
  } else if (!ac_present_ && (battery_.low() || shutdown_at_ms_ != 0)) {
    state_ = ups::State::LowBattery;
  } else if (!ac_present_) {
    state_ = ups::State::OnBattery;
  } else {
    state_ = ups::State::Online;
  }
}

bool App::readAcRaw(const SensorSample& m) const {
  const bool vin_ok = m.vin_mv >= store_.settings().vin_present_mv;
#if UPS_HAS_AC_OK_PIN
  const int level = digitalRead(brd::PIN_AC_OK);
  const bool pin_ok = (UPS_AC_OK_ACTIVE_HIGH != 0) ? (level == HIGH) : (level == LOW);
  return pin_ok && vin_ok;
#else
  return vin_ok;
#endif
}

bool App::readChargePin(uint8_t pin) const {
  const bool level = digitalRead(pin) == HIGH;
  return (UPS_CHG_STATUS_ACTIVE_LOW != 0) ? !level : level;
}

bool App::chargeStatusActive() const {
#if UPS_HAS_CHG_STAT
  return readChargePin(brd::PIN_CHG_STAT);
#else
  return false;
#endif
}

bool App::chargeDoneActive() const {
#if UPS_HAS_CHG_DONE
  return readChargePin(brd::PIN_CHG_DONE);
#else
  return false;
#endif
}

bool App::chargerStatusWired() const {
  return (UPS_HAS_CHG_STAT != 0) || (UPS_HAS_CHG_DONE != 0);
}

bool App::testSupported() const {
  return UPS_HAS_AC_CUT != 0;
}

bool App::startTest(uint32_t seconds, bool until_low, char* err, size_t cap) {
#if !UPS_HAS_AC_CUT
  (void)seconds;
  (void)until_low;
  appendError(err, cap, "test_not_supported");
  return false;
#else
  if (!ac_present_) {
    appendError(err, cap, "no_ac");
    return false;
  }
  if (!battery_present_) {
    appendError(err, cap, "no_battery");
    return false;
  }
  if (test_phase_ != TestPhase::Idle || test_active_) {
    appendError(err, cap, "test_busy");
    return false;
  }
  if (battery_.socX10() < 300) {
    appendError(err, cap, "battery_low");
    return false;
  }
  if (faults_ != 0 || output_cut_) {
    appendError(err, cap, "not_ready");
    return false;
  }

  const uint32_t now = millis();
  test_phase_ = TestPhase::BoostPre;
  test_phase_ms_ = now;
  test_until_low_ = until_low;
  uint32_t duration_s = until_low ? (TEST_MAX_UNTIL_LOW_MS / 1000u) : seconds;
  if (duration_s > (TEST_MAX_UNTIL_LOW_MS / 1000u)) {
    duration_s = TEST_MAX_UNTIL_LOW_MS / 1000u;
  }
  test_deadline_ms_ = now + duration_s * 1000u;
  test_abort_ = false;
  test_active_ = false;
  ac_cut_on_ = false;
  boost_on_ = true;
  setPin(brd::PIN_BOOST_EN, true, UPS_BOOST_EN_ACTIVE_HIGH != 0);
  return true;
#endif
}

void App::stopTest() {
  if (test_phase_ == TestPhase::Idle) {
    return;
  }
  if (test_phase_ == TestPhase::RestoreAc) {
    return;
  }
  test_abort_ = true;
  test_phase_ = TestPhase::RestoreAc;
  test_phase_ms_ = millis();
  test_active_ = false;
}

void App::latchFault(uint16_t bits, uint32_t now, uint32_t retry_ms) {
  if ((latched_faults_ & bits) != 0) {
    return;
  }
  latched_faults_ |= bits;
  last_fault_ms_ = now;
  if (fault_retry_count_ < 255) {
    ++fault_retry_count_;
  }
  if (fault_retry_count_ >= MAX_FAULT_RETRIES) {
    fault_retry_ms_ = 0;
  } else {
    fault_retry_ms_ = now + retry_ms;
  }
}

void App::clearLatchedFaults() {
  latched_faults_ = 0;
  fault_retry_ms_ = 0;
  fault_retry_count_ = 0;
  overcurrent_ms_ = 0;
  output_bad_ms_ = 0;
  charger_stale_ms_ = 0;
}

void App::requestStateSave(uint32_t now) {
  const uint32_t due = now + STATE_SAVE_DELAY_MS;
  if (next_state_save_ms_ == 0 ||
      static_cast<int32_t>(due - next_state_save_ms_) < 0) {
    next_state_save_ms_ = due;
  }
}

void App::saveStateIfDue(uint32_t now) {
  if (next_state_save_ms_ == 0) {
    return;
  }
  if (static_cast<int32_t>(now - next_state_save_ms_) < 0) {
    return;
  }
  if (test_phase_ != TestPhase::Idle) {
    next_state_save_ms_ = now + 60000;
    return;
  }

  RuntimeState& st = store_.stateMutable();
  st.soc_x10 = battery_.socX10();
  st.ac_fail_count = ac_fail_count_;
  st.cycle_count = cycle_count_;
  st.total_runtime_s = total_runtime_s_;
  if (faults_ != 0) {
    st.last_fault = faults_;
  }

  if (store_.save()) {
    last_save_ms_ = now;
    next_state_save_ms_ = 0;
    nvm_save_failed_ = false;
  } else {
    next_state_save_ms_ = now + STATE_SAVE_RETRY_MS;
    nvm_save_failed_ = true;
  }
}

void App::updateRuntime(uint32_t now) {
  const uint32_t second = now / 1000u;
  if (second == last_second_s_) {
    return;
  }
  last_second_s_ = second;
  uptime_s_ = (now - boot_ms_) / 1000u;

  if (!ac_present_ && output_on_) {
    ++total_runtime_s_;
    store_.stateMutable().total_runtime_s = total_runtime_s_;
  }
  if (now - last_save_ms_ >= STATE_SAVE_INTERVAL_MS) {
    requestStateSave(now);
  }
}

void App::buildUiInput(UiInput& in) const {
  in.state = state_;
  in.ac_present = ac_present_;
  in.on_battery = !ac_present_ && output_on_;
  in.low_battery = battery_.low() || battery_.critical();
  in.warning = warnings_ != 0;
  in.fault = faults_ != 0;
  in.test_active = test_active_;
  in.beeper_enabled = store_.settings().beeper_enabled != 0;
}

void App::buildTelemetry(ups::Telemetry& t) const {
  t = ups::Telemetry{};
  const SensorSample& m = sensors_.sample();
  const Settings& s = store_.settings();

  t.uptime_s = uptime_s_;
  t.ac_fail_count = ac_fail_count_;
  t.vin_mv = m.vin_mv;
  t.vin_fault_mv = vin_fault_mv_;
  t.vout_mv = m.vout_mv;
  t.vbat_mv = m.vbat_mv;
  t.ibat_ma = m.ibat_ma;
  t.iout_ma = iout_ma_;
  t.temp_c_x10 = m.temp_c_x10;
  t.soc_x10 = battery_present_ ? battery_.socX10() : 0;
  t.runtime_s = runtime_s_;
  t.load_pct = load_pct_;
  t.faults = faults_;
  t.warnings = warnings_;
  t.state = static_cast<uint8_t>(state_);
  t.battery_low = battery_.low();
  t.critical_battery = battery_.critical();
  t.ina_ok = m.ina_ok;
  t.ntc_ok = m.ntc_ok;
  t.output_sensor_ok = m.iout_ok;
  t.config_dirty = store_.dirty();

  uint32_t now = millis();
  if (shutdown_at_ms_ != 0) {
    if (static_cast<int32_t>(shutdown_at_ms_ - now) > 0) {
      t.shutdown_remaining_s = (shutdown_at_ms_ - now + 999u) / 1000u;
    }
  }
  if (restore_deadline_ms_ != 0) {
    if (static_cast<int32_t>(restore_deadline_ms_ - now) > 0) {
      t.restore_remaining_s = (restore_deadline_ms_ - now + 999u) / 1000u;
    }
  }

  uint8_t flags = 0;
  if (ac_present_) flags |= ups::TF_AC_PRESENT;
  if (output_on_) flags |= ups::TF_OUTPUT_ON;
  if (charging_) flags |= ups::TF_CHARGING;
  if (charge_done_) flags |= ups::TF_CHARGE_DONE;
  if (battery_present_) flags |= ups::TF_BATTERY_PRESENT;
  if (test_active_) flags |= ups::TF_TEST_ACTIVE;
  if (shutdown_at_ms_ != 0 || output_cut_) flags |= ups::TF_SHUTDOWN_PENDING;
  if (s.beeper_enabled) flags |= ups::TF_BEEPER_ENABLED;
  t.flags = flags;
}

void App::commGetTelemetry(ups::Telemetry& out) {
  buildTelemetry(out);
}

const Settings& App::commSettings() const {
  return store_.settings();
}

bool App::commApplySetting(const char* key, const char* value, char* err, size_t cap) {
  char local[48] = {0};
  if (!store_.apply(key, value, local, sizeof(local))) {
    appendError(err, cap, local[0] != '\0' ? local : "invalid_value");
    return false;
  }
  return true;
}

bool App::commSaveSettings(char* err, size_t cap) {
  RuntimeState& st = store_.stateMutable();
  st.soc_x10 = battery_.socX10();
  st.ac_fail_count = ac_fail_count_;
  st.cycle_count = cycle_count_;
  st.total_runtime_s = total_runtime_s_;
  if (!store_.save()) {
    nvm_save_failed_ = true;
    appendError(err, cap, "nvm_write_failed");
    return false;
  }
  nvm_save_failed_ = false;
  last_save_ms_ = millis();
  next_state_save_ms_ = 0;
  return true;
}

bool App::commFactoryReset(char* err, size_t cap) {
  if (!store_.factoryReset()) {
    nvm_save_failed_ = true;
    appendError(err, cap, "nvm_write_failed");
    return false;
  }
  ac_fail_count_ = 0;
  cycle_count_ = 0;
  total_runtime_s_ = 0;
  nvm_save_failed_ = false;
  last_save_ms_ = millis();
  next_state_save_ms_ = 0;
  battery_.setSocX10(store_.state().soc_x10);
  return true;
}

bool App::commRequestShutdown(uint32_t after_s, uint32_t restore_s,
                              char* err, size_t cap) {
  if (after_s > 86400ul || restore_s > 604800ul) {
    appendError(err, cap, "time_out_of_range");
    return false;
  }
  if (test_phase_ != TestPhase::Idle) {
    stopTest();
  }
  const uint32_t now = millis();
  shutdown_reason_ = ShutdownReason::Host;
  shutdown_at_ms_ = now + after_s * 1000u;
  shutdown_restore_s_ = restore_s;
  restore_not_before_ms_ = 0;
  if (after_s == 0) {
    shutdown_at_ms_ = now;
  }
  requestStateSave(now);
  return true;
}

void App::commCancelShutdown() {
  shutdown_at_ms_ = 0;
  shutdown_restore_s_ = 0;
  shutdown_reason_ = ShutdownReason::None;
  restore_not_before_ms_ = 0;

  if (output_cut_) {
    // Keep the output off for at least 10 seconds, as required by the Megatec
    // shutdown/restore sequence, before updateShutdown() releases it.
    restore_wait_ac_ = false;
    restore_deadline_ms_ = millis() + RESTORE_AFTER_AC_MS;
    requestStateSave(millis());
  } else {
    restore_wait_ac_ = false;
    restore_deadline_ms_ = 0;
  }
}

bool App::commRequestTest(uint32_t seconds, bool until_low,
                          char* err, size_t cap) {
  if (!testSupported()) {
    appendError(err, cap, "test_not_supported");
    return false;
  }
  return startTest(seconds, until_low, err, cap);
}

void App::commCancelTest() {
  stopTest();
}

void App::commToggleBeeper() {
  const bool enabled = store_.settings().beeper_enabled != 0;
  char err[32] = {0};
  store_.apply("beeper_enabled", enabled ? "0" : "1", err, sizeof(err));
}

void App::setPin(uint8_t pin, bool on, bool active_high) {
  digitalWrite(pin, (on == active_high) ? HIGH : LOW);
}

void App::watchdogBegin() {
#if UPS_WATCHDOG
  // LSI ~40 kHz / 64 = 625 Hz; 1250 counts ~= 2 s.
  IWDG->KR = 0x5555u;
  IWDG->PR = 4u;
  IWDG->RLR = 1250u;
  IWDG->KR = 0xAAAAu;
  IWDG->KR = 0xCCCCu;
#endif
}

void App::watchdogFeed() {
#if UPS_WATCHDOG
  IWDG->KR = 0xAAAAu;
#endif
}

void App::debugPrint(uint32_t now) {
#if UPS_DEBUG_UART
  if (now - last_debug_ms_ < 5000u) {
    return;
  }
  last_debug_ms_ = now;
  Serial1.print("ups st=");
  Serial1.print(static_cast<int>(state_));
  Serial1.print(" ac=");
  Serial1.print(ac_present_ ? 1 : 0);
  Serial1.print(" out=");
  Serial1.print(output_on_ ? 1 : 0);
  Serial1.print(" vin=");
  Serial1.print(sensors_.sample().vin_mv);
  Serial1.print(" vout=");
  Serial1.print(sensors_.sample().vout_mv);
  Serial1.print(" vbat=");
  Serial1.print(sensors_.sample().vbat_mv);
  Serial1.print(" ibat=");
  Serial1.print(sensors_.sample().ibat_ma);
  Serial1.print(" soc=");
  Serial1.print(battery_.socX10());
  Serial1.print(" tmp=");
  Serial1.print(sensors_.sample().temp_c_x10);
  Serial1.print(" f=");
  Serial1.print(faults_);
  Serial1.print(" w=");
  Serial1.println(warnings_);
#else
  (void)now;
#endif
}

int32_t App::clamp32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
