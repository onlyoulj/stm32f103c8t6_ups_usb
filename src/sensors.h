#pragma once

#include <stdint.h>

#include "ina219.h"
#include "settings.h"

struct SensorSample {
  uint32_t timestamp_ms;
  int32_t vin_mv;
  int32_t vout_mv;
  int32_t vbat_mv;
  int32_t ibat_ma;    // positive = charging
  int32_t iout_ma;

  int16_t temp_c_x10;

  bool vin_ok;
  bool vout_ok;
  bool vbat_ok;
  bool ntc_ok;
  bool ina_ok;
  bool iout_ok;
  bool vbat_from_ina;

  int32_t ina_bus_mv;  // raw INA219 bus measurement, useful for diagnostics
};

class Sensors {
 public:
  void begin(const Settings& s);
  void poll(const Settings& s, uint32_t now_ms);
  const SensorSample& sample() const { return sample_; }

 private:
  void readRails(const Settings& s);
  void readCurrent(const Settings& s, uint32_t now_ms);
  void readTemperature(const Settings& s, uint32_t now_ms);
  uint16_t readAdc(uint8_t pin);

  int32_t adc_vbat_mv_;
  uint16_t ntc_raw_mv_;
  bool temp_filter_valid_;
  int16_t temp_filtered_x10_;
  uint32_t last_adc_ms_;
  uint32_t last_ina_ms_;
  uint32_t last_ntc_ms_;

  Ina219 ina_bat_;
  Ina219 ina_out_;
  SensorSample sample_;
  uint16_t shunt_cfg_ = 0;
  bool invert_cfg_ = false;
};
