#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "battery.h"
#include "comm.h"
#include "config_store.h"
#include "sensors.h"
#include "ui.h"

class App : public CommHost {
 public:
  App();

  void begin();
  void loop();

  void commGetTelemetry(ups::Telemetry& out) override;
  const Settings& commSettings() const override;
  bool commApplySetting(const char* key, const char* value, char* err, size_t cap) override;
  bool commSaveSettings(char* err, size_t cap) override;
  bool commFactoryReset(char* err, size_t cap) override;
  bool commRequestShutdown(uint32_t after_s, uint32_t restore_s,
                           char* err, size_t cap) override;
  void commCancelShutdown() override;
  bool commRequestTest(uint32_t seconds, bool until_low,
                       char* err, size_t cap) override;
  void commCancelTest() override;
  void commToggleBeeper() override;

 private:
  enum class ShutdownReason : uint8_t { None = 0, Host, LowBattery };
  enum class TestPhase : uint8_t { Idle = 0, BoostPre, CutAc, Running, RestoreAc };

  void control(uint32_t now, uint32_t dt_ms);
  void updateAc(uint32_t now);
  void updateRuntime(uint32_t now);
  void updateProtections(uint32_t now, uint32_t dt_ms);
  void updateShutdown(uint32_t now);
  void updateTest(uint32_t now);
  void updateOutputs(uint32_t now);
  void updateState(uint32_t now);
  void handleButtonEvents(uint32_t now);
  void buildUiInput(UiInput& in) const;
  void buildTelemetry(ups::Telemetry& t) const;

  bool readAcRaw(const SensorSample& m) const;
  bool readChargePin(uint8_t pin) const;
  bool chargeStatusActive() const;
  bool chargeDoneActive() const;
  bool chargerStatusWired() const;
  bool testSupported() const;

  bool startTest(uint32_t seconds, bool until_low, char* err, size_t cap);
  void stopTest();

  void latchFault(uint16_t bits, uint32_t now, uint32_t retry_ms);
  void clearLatchedFaults();
  void requestStateSave(uint32_t now);
  void saveStateIfDue(uint32_t now);
  void setPin(uint8_t pin, bool on, bool active_high);
  void watchdogBegin();
  void watchdogFeed();
  void debugPrint(uint32_t now);
  static int32_t clamp32(int32_t v, int32_t lo, int32_t hi);

  ConfigStore store_;
  Sensors sensors_;
  BatteryModel battery_;
  Ui ui_;
  Protocol comm_;

  ups::State state_;
  bool ac_present_;
  bool ac_raw_last_;
  uint32_t ac_raw_change_ms_;
  uint32_t boot_ms_;
  uint32_t last_control_ms_;
  uint32_t last_debug_ms_;
  uint32_t last_second_s_;
  uint32_t last_save_ms_;
  uint32_t next_state_save_ms_;
  uint32_t uptime_s_;
  uint32_t total_runtime_s_;
  uint32_t ac_fail_count_;
  uint32_t cycle_count_;

  bool output_on_;
  bool boost_on_;
  bool charge_on_;
  bool fan_on_;
  bool ac_cut_on_;
  bool charging_;
  bool charge_done_;
  bool battery_present_;
  bool battery_ov_;
  bool charge_temp_ok_;

  int32_t vin_fault_mv_;
  int32_t last_good_vin_mv_;
  int32_t iout_ma_;
  int32_t load_ma_;
  uint16_t load_pct_;
  uint32_t runtime_s_;

  uint16_t faults_;
  uint16_t warnings_;
  uint16_t latched_faults_;
  uint32_t fault_retry_ms_;
  uint32_t last_fault_ms_;
  uint8_t fault_retry_count_;
  uint32_t overcurrent_ms_;
  uint32_t output_bad_ms_;
  uint32_t output_on_ms_;
  uint32_t output_check_grace_until_;
  uint32_t charge_oc_block_until_;
  uint32_t charger_stale_ms_;
  uint32_t boost_off_at_ms_;
  bool nvm_save_failed_;

  ShutdownReason shutdown_reason_;
  uint32_t shutdown_at_ms_;
  uint32_t shutdown_restore_s_;
  bool output_cut_;
  bool restore_wait_ac_;
  uint32_t restore_deadline_ms_;
  uint32_t restore_not_before_ms_;

  TestPhase test_phase_;
  uint32_t test_phase_ms_;
  uint32_t test_deadline_ms_;
  bool test_active_;
  bool test_until_low_;
  bool test_abort_;
};
