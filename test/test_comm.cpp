#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "comm.h"

class FakeStream : public Stream {
 public:
  std::string input;
  std::string output;

  int available() override { return static_cast<int>(input.size()); }

  int read() override {
    if (input.empty()) {
      return -1;
    }
    const unsigned char c = static_cast<unsigned char>(input.front());
    input.erase(input.begin());
    return c;
  }

  size_t write(const uint8_t* data, size_t len) override {
    output.append(reinterpret_cast<const char*>(data), len);
    return len;
  }

  void push(const char* text) { input += text; }
};

class FakeHost : public CommHost {
 public:
  FakeHost() {
    settingsDefaults(settings_);
    memset(&telemetry_, 0, sizeof(telemetry_));
    telemetry_.uptime_s = 1234;
    telemetry_.state = static_cast<uint8_t>(ups::State::Online);
    telemetry_.flags = ups::TF_AC_PRESENT | ups::TF_OUTPUT_ON |
                       ups::TF_BATTERY_PRESENT | ups::TF_BEEPER_ENABLED;
    telemetry_.vin_mv = 12010;
    telemetry_.vin_fault_mv = 12010;
    telemetry_.vout_mv = 11980;
    telemetry_.vbat_mv = 7810;
    telemetry_.ibat_ma = -420;
    telemetry_.iout_ma = 760;
    telemetry_.temp_c_x10 = 253;
    telemetry_.soc_x10 = 873;
    telemetry_.runtime_s = 6100;
    telemetry_.load_pct = 38;
    telemetry_.ina_ok = true;
    telemetry_.ntc_ok = true;
    telemetry_.output_sensor_ok = true;
  }

  void commGetTelemetry(ups::Telemetry& out) override { out = telemetry_; }
  const Settings& commSettings() const override { return settings_; }

  bool commApplySetting(const char* key, const char* value, char* err, size_t cap) override {
    if (strcmp(key, "low_pct") == 0) {
      settings_.low_pct = static_cast<uint16_t>(atoi(value));
      return true;
    }
    if (err != nullptr && cap > 0) {
      snprintf(err, cap, "unknown_key");
    }
    return false;
  }

  bool commSaveSettings(char* err, size_t cap) override {
    (void)err;
    (void)cap;
    ++saves;
    return true;
  }

  bool commFactoryReset(char* err, size_t cap) override {
    (void)err;
    (void)cap;
    ++resets;
    return true;
  }

  bool commRequestShutdown(uint32_t after_s, uint32_t restore_s,
                           char* err, size_t cap) override {
    (void)err;
    (void)cap;
    ++shutdowns;
    last_after_s = after_s;
    last_restore_s = restore_s;
    return true;
  }

  void commCancelShutdown() override { ++cancels; }

  bool commRequestTest(uint32_t seconds, bool until_low,
                       char* err, size_t cap) override {
    (void)err;
    (void)cap;
    ++tests;
    last_test_s = seconds;
    last_test_low = until_low;
    return true;
  }

  void commCancelTest() override { ++test_cancels; }
  void commToggleBeeper() override { ++beeps; }

  Settings settings_;
  ups::Telemetry telemetry_;
  int saves = 0;
  int resets = 0;
  int shutdowns = 0;
  int cancels = 0;
  int tests = 0;
  int test_cancels = 0;
  int beeps = 0;
  uint32_t last_after_s = 0;
  uint32_t last_restore_s = 0;
  uint32_t last_test_s = 0;
  bool last_test_low = false;
};

static void run(Protocol& proto, FakeStream& io, const char* command) {
  io.output.clear();
  io.push(command);
  for (int i = 0; i < 8 && io.available() > 0; ++i) {
    proto.poll();
  }
}

static bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

int main() {
  FakeStream io;
  FakeHost host;
  Protocol proto(io, host);

  run(proto, io, "Q1\r");
  assert(io.output.size() == 48);
  assert(io.output[0] == '(');
  assert(io.output[47] == '\r');
  assert(contains(io.output, "012.0 012.0 011.9"));
  assert(contains(io.output, "00001001\r"));
  printf("Q1: %s", io.output.c_str());

  run(proto, io, "F\r");
  assert(io.output == "#012.0 004 007.4 50.0\r");
  printf("F:  %s", io.output.c_str());

  run(proto, io, "I\r");
  assert(io.output.size() == 39);
  assert(io.output[0] == '#');
  printf("I:  %s", io.output.c_str());

  run(proto, io, "Q\r");
  assert(host.beeps == 1);
  assert(io.output.empty());

  run(proto, io, "S.3R0005\r");
  assert(host.shutdowns == 1);
  assert(host.last_after_s == 18);
  assert(host.last_restore_s == 300);
  assert(io.output.empty());

  run(proto, io, "S00R0000\r");
  assert(host.shutdowns == 2);
  assert(host.last_after_s == 0);
  assert(host.last_restore_s == 0);
  assert(io.output.empty());

  // Megatec R0001..R9999 minutes must fit the extended restore limit.
  run(proto, io, "S10R9999\r");
  assert(host.shutdowns == 3);
  assert(host.last_after_s == 600);
  assert(host.last_restore_s == 599940);
  assert(io.output.empty());

  run(proto, io, "C\r");
  assert(host.cancels == 1);

  run(proto, io, "T\r");
  assert(host.tests == 1);
  assert(host.last_test_s == 10);
  assert(!host.last_test_low);
  assert(io.output.empty());

  run(proto, io, "TL\r");
  assert(host.tests == 2);
  assert(host.last_test_low);
  assert(io.output.empty());

  run(proto, io, "T30\r");
  assert(host.tests == 3);
  assert(host.last_test_s == 1800);
  assert(io.output.empty());

  run(proto, io, "CT\r");
  assert(host.test_cancels == 1);

  run(proto, io, "XSTATUS\r\n");
  assert(contains(io.output, "\"ok\":true"));
  assert(contains(io.output, "\"state\":\"online\""));
  assert(contains(io.output, "\"soc_x10\":873"));
  assert(contains(io.output, "\"vin_mv\":12010"));
  assert(io.output.rfind("\r\n", io.output.size() - 2) == io.output.size() - 2);
  printf("XSTATUS: %s", io.output.c_str());

  run(proto, io, "XCONFIG\r\n");
  assert(io.output.size() < 1400);
  assert(contains(io.output, "\"cells\":2"));
  assert(contains(io.output, "\"low_pct\":20"));
  printf("XCONFIG bytes: %zu\n", io.output.size());

  run(proto, io, "XSET low_pct=25\r\n");
  assert(contains(io.output, "\"ok\":true"));
  assert(host.settings_.low_pct == 25);
  printf("XSET: %s", io.output.c_str());

  run(proto, io, "XSET nope=1\r\n");
  assert(contains(io.output, "\"ok\":false"));
  assert(contains(io.output, "unknown_key"));

  run(proto, io, "XSAVE\r\n");
  assert(host.saves == 1);
  assert(contains(io.output, "\"ok\":true"));

  run(proto, io, "XRESET\r\n");
  assert(host.resets == 1);
  assert(contains(io.output, "\"reset\":true"));

  run(proto, io, "XSHUTDOWN 10 60\r\n");
  assert(host.shutdowns == 4);
  assert(host.last_after_s == 10);
  assert(host.last_restore_s == 60);
  assert(contains(io.output, "\"shutdown_in_s\":10"));

  run(proto, io, "XSHUTDOWN 0 604801\r\n");
  assert(host.shutdowns == 4);
  assert(contains(io.output, "time_out_of_range"));

  run(proto, io, "XCANCEL\r\n");
  assert(host.cancels == 2);

  run(proto, io, "XTEST 30\r\n");
  assert(host.tests == 4);
  assert(host.last_test_s == 30);
  assert(contains(io.output, "\"ok\":true"));

  run(proto, io, "XBEEP\r\n");
  assert(host.beeps == 2);
  assert(contains(io.output, "\"beeper\":true"));

  run(proto, io, "FOO\r");
  assert(io.output == "FOO\r");

  run(proto, io, "XSET broken\r\n");
  assert(contains(io.output, "missing_equals"));

  run(proto, io, "XTEST 0\r\n");
  assert(contains(io.output, "bad_test_duration"));

  printf("all protocol parser tests passed\n");
  return 0;
}
