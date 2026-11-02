#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "protocol_format.h"
#include "settings.h"

static void testSettings() {
  Settings s;
  settingsDefaults(s);
  char err[64] = {0};
  assert(settingsValidate(s, err, sizeof(err)));
  assert(s.cells == 2);
  assert(packFullMv(s) == 8400);
  assert(packLowMv(s) == 6900);
  assert(settingsCrc(s) == s.crc);

  Settings t = s;
  t.capacity_mAh = 6000;
  settingsFinalize(t);
  assert(t.crc != s.crc);

  Settings u = s;
  assert(settingsApply(u, "cells", "3", err, sizeof(err)));
  assert(settingsValidate(u, err, sizeof(err)));
  assert(u.cells == 3);

  Settings bad = s;
  assert(settingsApply(bad, "low_pct", "5", err, sizeof(err)));
  assert(!settingsValidate(bad, err, sizeof(err)));

  assert(!settingsApply(bad, "not_a_key", "1", err, sizeof(err)));
  printf("settings: ok\n");
}

static void testQ1() {
  ups::Q1Status q = {};
  q.vin_mv = 12000;
  q.vin_fault_mv = 12000;
  q.vout_mv = 12000;
  q.vbat_mv = 7800;
  q.load_pct = 30;
  q.freq_x10 = 500;
  q.temp_c_x10 = 253;
  q.standby = true;
  q.beeper_on = true;

  char buf[64];
  size_t n = ups::formatQ1(q, buf, sizeof(buf));
  assert(n == 48);
  assert(strcmp(buf, "(012.0 012.0 012.0 030 50.0 07.80 25.3 00001001\r") == 0);
  printf("q1 online: %s", buf);

  q.utility_fail = true;
  q.battery_low = true;
  q.boost_active = true;
  q.ups_failed = true;
  q.test_active = true;
  q.shutdown_active = true;
  n = ups::formatQ1(q, buf, sizeof(buf));
  assert(n == 48);
  assert(strstr(buf, "11111111\r") != NULL);
  printf("q1 fault:  %s", buf);

  Settings s;
  settingsDefaults(s);
  n = ups::formatRating(s, buf, sizeof(buf));
  assert(n == 22);
  assert(strcmp(buf, "#012.0 004 007.4 50.0\r") == 0);
  printf("rating:    %s", buf);

  n = ups::formatVendor(buf, sizeof(buf));
  assert(n == 39);
  assert(buf[0] == '#');
  assert(buf[15] == ' ');
  assert(buf[16] == ' ');
  assert(buf[26] == 'V');
  assert(buf[27] == ' ');
  assert(buf[37] == ' ');
  assert(buf[38] == '\r');
  printf("vendor:    %s", buf);
  printf("protocol format: ok\n");
}

static void testBattery() {
  Settings s;
  settingsDefaults(s);
  BatteryModel b;
  b.begin(s, 500, true);
  assert(b.socX10() == 500);

  // One hour at 1000 mA discharge from a 5000 mAh pack: -20 %.
  for (int i = 0; i < 3600; ++i) {
    b.update(s, 7400, -1000, true, false, false, false, true, 1000);
  }
  assert(b.socX10() >= 298 && b.socX10() <= 302);
  assert(!b.low());
  assert(!b.critical());

  b.setSocX10(500);
  assert(b.runtimeSeconds(s, 1000) == 9000);
  assert(b.remaining_mAh(s) == 2500);

  b.setSocX10(150);
  b.update(s, 7000, -100, true, false, false, false, true, 6000);
  assert(b.low());
  assert(!b.critical());

  b.setSocX10(40);
  b.update(s, 6500, -100, true, false, false, false, true, 100);
  assert(b.low());
  assert(b.critical());

  b.setSocX10(1000);
  b.update(s, 8350, 0, true, true, true, true, false, 100);
  assert(b.full());

  assert(BatteryModel::ocvSocX10(s, 7400) == 400);
  assert(BatteryModel::ocvSocX10(s, 8400) == 1000);
  assert(BatteryModel::ocvSocX10(s, 6000) == 0);

  // A low-voltage debounce accumulated during discharge must clear once the
  // charger starts; otherwise low() would remain asserted after recovery.
  BatteryModel c;
  c.begin(s, 500, true);
  for (int i = 0; i < 6; ++i) {
    c.update(s, 6800, -100, true, false, false, false, true, 1000);
  }
  assert(c.low());
  c.setSocX10(500);
  c.update(s, 8400, 100, true, true, true, false, false, 1000);
  assert(!c.low());
  assert(!c.critical());
  printf("battery model: ok\n");
}

int main() {
  testSettings();
  testQ1();
  testBattery();
  printf("all host tests passed\n");
  return 0;
}
