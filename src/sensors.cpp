#include "sensors.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

#include "board.h"

namespace {

template <uint8_t N>
class Average {
 public:
  void reset() {
    memset(buf_, 0, sizeof(buf_));
    index_ = 0;
    count_ = 0;
    sum_ = 0;
  }

  int32_t add(int32_t value) {
    if (count_ < N) {
      buf_[count_++] = value;
      sum_ += value;
    } else {
      sum_ -= buf_[index_];
      buf_[index_] = value;
      sum_ += value;
      index_ = static_cast<uint8_t>((index_ + 1) % N);
    }
    return count_ == 0 ? value : static_cast<int32_t>(sum_ / count_);
  }

 private:
  int32_t buf_[N] = {0};
  uint8_t index_ = 0;
  uint8_t count_ = 0;
  int64_t sum_ = 0;
};

Average<8> g_vbat_filter;
Average<8> g_vin_filter;
Average<8> g_vout_filter;
Average<8> g_ntc_filter;

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

int32_t scaleRail(int32_t pin_mv, int32_t num, int32_t den, int16_t scale, int16_t offset) {
  int64_t mv = (static_cast<int64_t>(pin_mv) * num) / den;
  mv = (mv * scale) / 1000;
  mv += offset;
  return clamp32(static_cast<int32_t>(mv), 0, 60000);
}

}  // namespace

void Sensors::begin(const Settings& s) {
  memset(&sample_, 0, sizeof(sample_));
  sample_.temp_c_x10 = TEMP_INVALID_C_X10;
  sample_.timestamp_ms = millis();

  pinMode(brd::PIN_VBAT, INPUT_ANALOG);
  pinMode(brd::PIN_VIN, INPUT_ANALOG);
  pinMode(brd::PIN_VOUT, INPUT_ANALOG);
  pinMode(brd::PIN_NTC, INPUT_ANALOG);
  analogReadResolution(12);

  g_vbat_filter.reset();
  g_vin_filter.reset();
  g_vout_filter.reset();
  g_ntc_filter.reset();

  Wire.begin();
  Wire.setClock(100000);
  shunt_cfg_ = s.shunt_mohm;
  invert_cfg_ = s.ina_invert != 0;
  ina_bat_.begin(0x40, shunt_cfg_, invert_cfg_);
#if UPS_HAS_OUTPUT_INA219
  ina_out_.begin(0x41, shunt_cfg_, invert_cfg_);
#endif

  last_adc_ms_ = 0;
  last_ina_ms_ = 0;
  last_ntc_ms_ = 0;
  temp_filter_valid_ = false;
  temp_filtered_x10_ = 0;
}

void Sensors::poll(const Settings& s, uint32_t now_ms) {
  if (s.shunt_mohm != shunt_cfg_ || (s.ina_invert != 0) != invert_cfg_) {
    shunt_cfg_ = s.shunt_mohm;
    invert_cfg_ = s.ina_invert != 0;
    ina_bat_.begin(0x40, shunt_cfg_, invert_cfg_);
#if UPS_HAS_OUTPUT_INA219
    ina_out_.begin(0x41, shunt_cfg_, invert_cfg_);
#endif
  }

  if (now_ms - last_adc_ms_ >= 20) {
    last_adc_ms_ = now_ms;
    readRails(s);
  }
  if (now_ms - last_ntc_ms_ >= 100) {
    last_ntc_ms_ = now_ms;
    readTemperature(s, now_ms);
  }
  if (now_ms - last_ina_ms_ >= 250) {
    last_ina_ms_ = now_ms;
    readCurrent(s, now_ms);
  }
  sample_.timestamp_ms = now_ms;
}

void Sensors::readRails(const Settings& s) {
  const uint16_t vbat_raw = readAdc(brd::PIN_VBAT);
  const uint16_t vin_raw = readAdc(brd::PIN_VIN);
  const uint16_t vout_raw = readAdc(brd::PIN_VOUT);
  ntc_raw_mv_ = static_cast<uint16_t>(
      static_cast<uint32_t>(readAdc(brd::PIN_NTC)) * ADC_VREF_MV / ADC_MAX_COUNTS);

  const int32_t vbat_mv = g_vbat_filter.add(
      scaleRail(static_cast<int32_t>(vbat_raw) * ADC_VREF_MV / ADC_MAX_COUNTS,
                UPS_VBAT_DIV_NUM, UPS_VBAT_DIV_DEN, s.vbat_scale_permille, s.vbat_offset_mv));
  const int32_t vin_mv = g_vin_filter.add(
      scaleRail(static_cast<int32_t>(vin_raw) * ADC_VREF_MV / ADC_MAX_COUNTS,
                UPS_VIN_DIV_NUM, UPS_VIN_DIV_DEN, s.vin_scale_permille, s.vin_offset_mv));
  const int32_t vout_mv = g_vout_filter.add(
      scaleRail(static_cast<int32_t>(vout_raw) * ADC_VREF_MV / ADC_MAX_COUNTS,
                UPS_VOUT_DIV_NUM, UPS_VOUT_DIV_DEN, s.vout_scale_permille, s.vout_offset_mv));

  adc_vbat_mv_ = vbat_mv;
  sample_.vbat_mv = vbat_mv;
  sample_.vin_mv = vin_mv;
  sample_.vout_mv = vout_mv;

  sample_.vbat_ok = (vbat_raw > 2) && (vbat_raw < 4090);
  sample_.vin_ok = vin_raw < 4090;
  sample_.vout_ok = vout_raw < 4090;
  sample_.vbat_from_ina = false;

  if (!sample_.vbat_ok && sample_.ina_ok && sample_.ina_bus_mv > 3000) {
    sample_.vbat_mv = sample_.ina_bus_mv;
    sample_.vbat_from_ina = true;
  }
}

void Sensors::readCurrent(const Settings& s, uint32_t now_ms) {
  ina_bat_.retry(now_ms);
  if (ina_bat_.present()) {
    if (ina_bat_.read()) {
      sample_.ina_ok = true;
      sample_.ina_bus_mv = ina_bat_.busMv();
      sample_.ibat_ma = ina_bat_.currentMa();
    } else {
      sample_.ina_ok = false;
      sample_.ibat_ma = 0;
    }
  } else {
    sample_.ina_ok = false;
    sample_.ibat_ma = 0;
    sample_.ina_bus_mv = 0;
  }

#if UPS_HAS_OUTPUT_INA219
  ina_out_.retry(now_ms);
  if (ina_out_.present() && ina_out_.read()) {
    sample_.iout_ma = ina_out_.currentMa();
    sample_.iout_ok = true;
  } else {
    sample_.iout_ma = 0;
    sample_.iout_ok = false;
  }
#else
  (void)s;
  sample_.iout_ma = 0;
  sample_.iout_ok = false;
#endif
}

void Sensors::readTemperature(const Settings& s, uint32_t now_ms) {
  (void)now_ms;
  if (!s.ntc_enabled) {
    sample_.ntc_ok = false;
    sample_.temp_c_x10 = TEMP_INVALID_C_X10;
    return;
  }

  const int32_t pin_mv = g_ntc_filter.add(static_cast<int32_t>(ntc_raw_mv_));
  if (pin_mv <= 60 || pin_mv >= (static_cast<int32_t>(ADC_VREF_MV) - 60)) {
    sample_.ntc_ok = false;
    sample_.temp_c_x10 = TEMP_INVALID_C_X10;
    temp_filter_valid_ = false;
    return;
  }

  // 3.3V -> 10k -> ADC -> NTC -> GND.  Rntc = Rseries * V / (Vref - V).
  const int64_t denom = static_cast<int64_t>(ADC_VREF_MV) - pin_mv;
  if (denom <= 0) {
    sample_.ntc_ok = false;
    sample_.temp_c_x10 = TEMP_INVALID_C_X10;
    return;
  }
  const int64_t r_ntc = static_cast<int64_t>(NTC_SERIES_OHM) * pin_mv / denom;
  if (r_ntc < 200 || r_ntc > 1000000) {
    sample_.ntc_ok = false;
    sample_.temp_c_x10 = TEMP_INVALID_C_X10;
    temp_filter_valid_ = false;
    return;
  }

  const float ratio = static_cast<float>(r_ntc) / static_cast<float>(NTC_R25_OHM);
  const float inv_t = (1.0f / 298.15f) + (logf(ratio) / static_cast<float>(NTC_BETA));
  const float temp_c = (1.0f / inv_t) - 273.15f + (static_cast<float>(s.temp_offset_c_x10) / 10.0f);
  int32_t temp_x10 = static_cast<int32_t>(temp_c * 10.0f + (temp_c >= 0 ? 0.5f : -0.5f));
  temp_x10 = clamp32(temp_x10, -400, 1000);

  if (!temp_filter_valid_) {
    temp_filtered_x10_ = static_cast<int16_t>(temp_x10);
    temp_filter_valid_ = true;
  } else {
    temp_filtered_x10_ = static_cast<int16_t>((static_cast<int32_t>(temp_filtered_x10_) * 3 + temp_x10) / 4);
  }

  sample_.ntc_ok = true;
  sample_.temp_c_x10 = temp_filtered_x10_;
}

uint16_t Sensors::readAdc(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    sum += analogRead(pin);
  }
  return static_cast<uint16_t>((sum + 4) / 8);
}
