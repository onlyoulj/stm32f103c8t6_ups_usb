#pragma once

#include <stdint.h>

/*
 * Minimal INA219 high-side current/voltage monitor.
 *
 * The chip is configured for continuous shunt+bus conversion, 32 V bus range,
 * +/-320 mV shunt range and 12-bit conversion.  Current is derived from the
 * raw shunt voltage so the calibration register is not used and the same code
 * works with any shunt value.
 */
class Ina219 {
 public:
  bool begin(uint8_t address, uint16_t shunt_mohm, bool invert);
  bool read();
  void retry(uint32_t now_ms);

  bool present() const { return present_; }
  int32_t busMv() const { return bus_mv_; }
  int32_t currentMa() const { return current_ma_; }
  uint16_t shuntMohm() const { return shunt_mohm_; }

 private:
  bool readReg16(uint8_t reg, uint16_t& value);
  bool writeReg16(uint8_t reg, uint16_t value);

  uint8_t address_;
  uint16_t shunt_mohm_;
  bool invert_;
  bool present_;
  uint8_t fail_count_;
  uint32_t last_retry_ms_;
  int32_t bus_mv_;
  int32_t current_ma_;
};
