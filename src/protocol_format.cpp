#include "protocol_format.h"

#include <stdio.h>
#include <string.h>

namespace ups {
namespace {

void formatMilli1(int32_t mv, int int_digits, char* out, size_t cap) {
  if (mv < 0) {
    mv = 0;
  }
  int32_t ip = mv / 1000;
  int32_t frac = (mv % 1000) / 100;
  snprintf(out, cap, "%0*ld.%01ld", int_digits, (long)ip, (long)frac);
}

void formatMilli2(int32_t mv, int int_digits, char* out, size_t cap) {
  if (mv < 0) {
    mv = 0;
  }
  int32_t ip = mv / 1000;
  int32_t frac = (mv % 1000) / 10;
  snprintf(out, cap, "%0*ld.%02ld", int_digits, (long)ip, (long)frac);
}

void formatTemp1(int16_t temp_c_x10, char* out, size_t cap) {
  int32_t t = temp_c_x10;
  bool neg = t < 0;
  if (t == TEMP_INVALID_C_X10) {
    snprintf(out, cap, "%s", " 0.0");
    return;
  }
  if (neg) {
    t = -t;
  }
  snprintf(out, cap, "%s%02ld.%01ld", neg ? "-" : "", (long)(t / 10), (long)(t % 10));
}

void formatFreq(uint16_t freq_x10, char* out, size_t cap) {
  snprintf(out, cap, "%02u.%01u", (unsigned)(freq_x10 / 10u), (unsigned)(freq_x10 % 10u));
}

void padField(char* dst, size_t width, const char* src) {
  size_t n = strlen(src);
  if (n > width) {
    n = width;
  }
  memcpy(dst, src, n);
  memset(dst + n, ' ', width - n);
}

}  // namespace

size_t formatQ1(const Q1Status& q, char* out, size_t cap) {
  if (out == nullptr || cap < 56) {
    return 0;
  }

  char vin[12];
  char vinf[12];
  char vout[12];
  char vbat[12];
  char temp[12];
  char freq[12];
  formatMilli1(q.vin_mv, 3, vin, sizeof(vin));
  formatMilli1(q.vin_fault_mv, 3, vinf, sizeof(vinf));
  formatMilli1(q.vout_mv, 3, vout, sizeof(vout));
  formatMilli2(q.vbat_mv, 2, vbat, sizeof(vbat));
  formatTemp1(q.temp_c_x10, temp, sizeof(temp));
  formatFreq(q.freq_x10, freq, sizeof(freq));

  uint16_t load = q.load_pct > 100 ? 100 : q.load_pct;

  char bits[9];
  bits[0] = q.utility_fail ? '1' : '0';
  bits[1] = q.battery_low ? '1' : '0';
  bits[2] = q.boost_active ? '1' : '0';
  bits[3] = q.ups_failed ? '1' : '0';
  bits[4] = q.standby ? '1' : '0';
  bits[5] = q.test_active ? '1' : '0';
  bits[6] = q.shutdown_active ? '1' : '0';
  bits[7] = q.beeper_on ? '1' : '0';
  bits[8] = '\0';

  int n = snprintf(out, cap, "(%s %s %s %03u %s %s %s %s\r", vin, vinf, vout,
                   (unsigned)load, freq, vbat, temp, bits);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t formatRating(const Settings& s, char* out, size_t cap) {
  if (out == nullptr || cap < 32) {
    return 0;
  }
  char vin[12];
  char vbat[12];
  char freq[12];
  formatMilli1(s.vin_nominal_mv, 3, vin, sizeof(vin));
  formatMilli1(packNominalMv(s), 3, vbat, sizeof(vbat));
  formatFreq(s.nominal_freq_x10, freq, sizeof(freq));

  uint32_t input_a = (static_cast<uint32_t>(s.max_load_ma) +
                      static_cast<uint32_t>(s.max_charge_ma) + 999u) / 1000u;
  if (input_a < 1) {
    input_a = 1;
  }
  if (input_a > 999) {
    input_a = 999;
  }
  int n = snprintf(out, cap, "#%s %03lu %s %s\r", vin, (unsigned long)input_a, vbat, freq);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t formatVendor(char* out, size_t cap) {
  if (out == nullptr || cap < 40) {
    return 0;
  }
  const char* mfr = UPS_MANUFACTURER;
  const char* model = "DC-UPS-12V";
  const char* fw = UPS_FW_VERSION;

  char m[16];
  char d[11];
  char f[11];
  padField(m, 15, mfr);
  padField(d, 10, model);
  padField(f, 10, fw);
  m[15] = '\0';
  d[10] = '\0';
  f[10] = '\0';

  int n = snprintf(out, cap, "#%s %s %s\r", m, d, f);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    return 0;
  }
  return static_cast<size_t>(n);
}

}  // namespace ups
