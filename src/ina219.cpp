#include "ina219.h"

#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_SHUNT = 0x01;
constexpr uint8_t REG_BUS = 0x02;
constexpr uint16_t CONFIG_32V_320MV_12BIT_CONT = 0x3FFF;
constexpr uint32_t RETRY_MS = 10000;
}  // namespace

bool Ina219::begin(uint8_t address, uint16_t shunt_mohm, bool invert) {
  address_ = address;
  shunt_mohm_ = shunt_mohm;
  invert_ = invert;
  present_ = false;
  fail_count_ = 0;
  bus_mv_ = 0;
  current_ma_ = 0;
  last_retry_ms_ = millis();

  if (shunt_mohm_ == 0) {
    return false;
  }

  // Reset, then configure.  Some clone modules need a short settle time.
  writeReg16(REG_CONFIG, 0x8000);
  delay(2);
  if (!writeReg16(REG_CONFIG, CONFIG_32V_320MV_12BIT_CONT)) {
    return false;
  }
  uint16_t cfg = 0;
  if (!readReg16(REG_CONFIG, cfg) || cfg != CONFIG_32V_320MV_12BIT_CONT) {
    return false;
  }
  present_ = true;
  return true;
}

void Ina219::retry(uint32_t now_ms) {
  if (present_ || shunt_mohm_ == 0) {
    return;
  }
  if (now_ms - last_retry_ms_ < RETRY_MS) {
    return;
  }
  last_retry_ms_ = now_ms;
  begin(address_, shunt_mohm_, invert_);
}

bool Ina219::read() {
  if (!present_ || shunt_mohm_ == 0) {
    return false;
  }
  uint16_t bus_raw = 0;
  uint16_t shunt_raw = 0;
  if (!readReg16(REG_BUS, bus_raw) || !readReg16(REG_SHUNT, shunt_raw)) {
    if (fail_count_ < 255) {
      ++fail_count_;
    }
    if (fail_count_ >= 3) {
      present_ = false;
    }
    return false;
  }
  fail_count_ = 0;

  // Bus voltage: 4 mV per LSB, bits 15..3.
  bus_mv_ = static_cast<int32_t>(bus_raw >> 3) * 4;

  // Shunt voltage: 10 uV per LSB, signed 16-bit.  uV / mOhm = mA.
  int32_t shunt_uv = static_cast<int16_t>(shunt_raw) * 10;
  int32_t ma = shunt_uv / static_cast<int32_t>(shunt_mohm_);
  if (invert_) {
    ma = -ma;
  }
  if (ma > 30000) {
    ma = 30000;
  } else if (ma < -30000) {
    ma = -30000;
  }
  current_ma_ = ma;
  return true;
}

bool Ina219::writeReg16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(address_);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Ina219::readReg16(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(address_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(address_, static_cast<uint8_t>(2)) != 2) {
    return false;
  }
  uint8_t hi = static_cast<uint8_t>(Wire.read());
  uint8_t lo = static_cast<uint8_t>(Wire.read());
  value = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  return true;
}
