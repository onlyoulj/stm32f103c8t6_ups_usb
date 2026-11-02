#pragma once

#include <stddef.h>
#include <stdint.h>

#include "settings.h"

namespace ups {

struct Q1Status {
  int32_t vin_mv;
  int32_t vin_fault_mv;
  int32_t vout_mv;
  int32_t vbat_mv;
  uint16_t load_pct;
  uint16_t freq_x10;
  int16_t temp_c_x10;

  bool utility_fail;
  bool battery_low;
  bool boost_active;
  bool ups_failed;
  bool standby;
  bool test_active;
  bool shutdown_active;
  bool beeper_on;
};

// Megatec Q1 reply, e.g.:
//   (012.0 012.0 012.0 030 50.0 07.80 25.3 00010001\r
// Return value is the string length, or 0 if the buffer is too small.
size_t formatQ1(const Q1Status& q, char* out, size_t cap);

// Megatec F (ratings) reply, e.g.:
//   #012.0 005 007.4 50.0\r
size_t formatRating(const Settings& s, char* out, size_t cap);

// Megatec I (vendor) reply, exactly 39 bytes including CR:
//   #XiaoBuUPS      DC-UPS-12V 1.0.0     \r
size_t formatVendor(char* out, size_t cap);

}  // namespace ups
