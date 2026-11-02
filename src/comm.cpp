#include "comm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol_format.h"

namespace {

bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

char lowerChar(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char upperChar(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool ieq(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    if (lowerChar(*a) != lowerChar(*b)) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

char* trim(char* s) {
  while (*s == ' ' || *s == '\t') {
    ++s;
  }
  char* end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
    --end;
  }
  *end = '\0';
  return s;
}

void copyLower(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src[i] != '\0' && i + 1 < cap; ++i) {
    dst[i] = lowerChar(src[i]);
  }
  dst[i] = '\0';
}

bool parseUnsigned(const char* text, uint32_t& value, const char** end_out) {
  if (text == nullptr || !isDigit(*text)) {
    return false;
  }
  char* end = nullptr;
  unsigned long v = strtoul(text, &end, 10);
  if (end == text || v > 0xFFFFFFFFul) {
    return false;
  }
  value = static_cast<uint32_t>(v);
  if (end_out != nullptr) {
    *end_out = end;
  }
  return true;
}

const char* stateName(uint8_t state) {
  switch (state) {
    case static_cast<uint8_t>(ups::State::Boot):
      return "boot";
    case static_cast<uint8_t>(ups::State::Online):
      return "online";
    case static_cast<uint8_t>(ups::State::OnBattery):
      return "battery";
    case static_cast<uint8_t>(ups::State::LowBattery):
      return "low_battery";
    case static_cast<uint8_t>(ups::State::Shutdown):
      return "shutdown";
    case static_cast<uint8_t>(ups::State::Fault):
      return "fault";
    default:
      return "unknown";
  }
}

}  // namespace

Protocol::Protocol(Stream& io, CommHost& host)
    : io_(io), host_(host), line_len_(0), line_overflow_(false),
      json_len_(0), json_overflow_(false) {
  line_[0] = '\0';
  json_[0] = '\0';
}

void Protocol::poll() {
  uint8_t budget = 64;
  while (budget-- > 0 && io_.available() > 0) {
    consume(static_cast<char>(io_.read()));
  }
}

void Protocol::consume(char c) {
  if (c == '\r' || c == '\n') {
    if (line_len_ > 0 && !line_overflow_) {
      line_[line_len_] = '\0';
      handleLine();
    }
    line_len_ = 0;
    line_overflow_ = false;
    return;
  }
  if (c == '\t') {
    c = ' ';
  }
  if (c < 0x20 || c > 0x7E) {
    return;
  }
  if (static_cast<size_t>(line_len_) + 1 >= sizeof(line_)) {
    line_overflow_ = true;
    return;
  }
  line_[line_len_++] = c;
}

void Protocol::handleLine() {
  char* line = trim(line_);
  if (*line == '\0') {
    return;
  }
  if (line[0] == 'X' || line[0] == 'x') {
    handleExtended(line);
  } else {
    handleMegatec(line);
  }
}

void Protocol::handleMegatec(char* line) {
  if (ieq(line, "Q1")) {
    ups::Telemetry t;
    host_.commGetTelemetry(t);
    const Settings& s = host_.commSettings();
    ups::Q1Status q;
    q.vin_mv = t.vin_mv;
    q.vin_fault_mv = t.vin_fault_mv;
    q.vout_mv = t.vout_mv;
    q.vbat_mv = t.vbat_mv;
    q.load_pct = t.load_pct;
    q.freq_x10 = s.nominal_freq_x10;
    q.temp_c_x10 = t.temp_c_x10;
    q.utility_fail = !ups::telemetryFlag(t, ups::TF_AC_PRESENT);
    // Only assert LB while running from the battery.  NUT/upsmon treats LB as
    // a shutdown trigger even when the utility is present, and a deeply
    // discharged pack that is currently recharging must not stop the host.
    q.battery_low = t.battery_low && q.utility_fail;
    q.boost_active = q.utility_fail &&
                     ups::telemetryFlag(t, ups::TF_OUTPUT_ON);
    q.ups_failed = t.faults != 0;
    q.standby = true;
    q.test_active = ups::telemetryFlag(t, ups::TF_TEST_ACTIVE);
    q.shutdown_active = ups::telemetryFlag(t, ups::TF_SHUTDOWN_PENDING) ||
                        t.state == static_cast<uint8_t>(ups::State::Shutdown);
    q.beeper_on = ups::telemetryFlag(t, ups::TF_BEEPER_ENABLED);

    char buf[64];
    size_t n = ups::formatQ1(q, buf, sizeof(buf));
    if (n > 0) {
      writeRaw(buf, n);
    } else {
      writeLine("?");
    }
    return;
  }

  if (ieq(line, "F")) {
    char buf[40];
    size_t n = ups::formatRating(host_.commSettings(), buf, sizeof(buf));
    if (n > 0) {
      writeRaw(buf, n);
    } else {
      writeLine("?");
    }
    return;
  }

  if (ieq(line, "I")) {
    char buf[48];
    size_t n = ups::formatVendor(buf, sizeof(buf));
    if (n > 0) {
      writeRaw(buf, n);
    } else {
      writeLine("?");
    }
    return;
  }

  if (ieq(line, "Q")) {
    host_.commToggleBeeper();
    return;
  }

  if (ieq(line, "C")) {
    host_.commCancelShutdown();
    return;
  }

  if (ieq(line, "CT")) {
    host_.commCancelTest();
    return;
  }

  if (ieq(line, "T")) {
    handleTestCommand(line, "");
    return;
  }

  if (ieq(line, "TL")) {
    handleTestCommand(line, "low");
    return;
  }

  if (line[0] == 'T' || line[0] == 't') {
    const char* p = line + 1;
    if (isDigit(*p)) {
      handleTestCommand(line, p);
      return;
    }
  }

  if (line[0] == 'S' || line[0] == 's') {
    handleShutdownCommand(line);
    return;
  }

  // Megatec devices echo invalid commands; NUT's driver uses that as a
  // failure signal for instant commands.
  writeLine(line);
}

void Protocol::handleShutdownCommand(const char* line) {
  char err[48] = {0};
  const char* p = line + 1;
  uint32_t seconds = 0;
  uint32_t restore_s = 0;

  if (*p == '.') {
    ++p;
    if (!isDigit(*p)) {
      writeLine(line);
      return;
    }
    seconds = static_cast<uint32_t>(*p - '0') * 6u;  // 0.1 minute
    ++p;
  } else {
    uint32_t minutes = 0;
    int count = 0;
    while (isDigit(*p) && count < 2) {
      minutes = minutes * 10u + static_cast<uint32_t>(*p - '0');
      ++p;
      ++count;
    }
    if (count == 0) {
      writeLine(line);
      return;
    }
    seconds = minutes * 60u;
  }

  if (*p == 'R' || *p == 'r') {
    ++p;
    uint32_t minutes = 0;
    int count = 0;
    while (isDigit(*p) && count < 4) {
      minutes = minutes * 10u + static_cast<uint32_t>(*p - '0');
      ++p;
      ++count;
    }
    if (count == 0) {
      writeLine(line);
      return;
    }
    restore_s = minutes * 60u;
  }
  if (*p != '\0') {
    writeLine(line);
    return;
  }

  if (!host_.commRequestShutdown(seconds, restore_s, err, sizeof(err))) {
    writeLine(line);  // no reply means success in the Megatec protocol
  }
}

void Protocol::handleTestCommand(const char* line, const char* args) {
  char err[48] = {0};
  uint32_t seconds = 0;
  bool until_low = false;

  if (ieq(args, "low")) {
    until_low = true;
  } else if (*args != '\0') {
    uint32_t value = 0;
    const char* end = nullptr;
    if (!parseUnsigned(args, value, &end) || *end != '\0' ||
        value < 1 || value > 99) {
      writeLine(line);
      return;
    }
    seconds = value * 60u;
  } else {
    seconds = 10;  // Megatec T = quick 10 second test
  }

  if (!host_.commRequestTest(seconds, until_low, err, sizeof(err))) {
    writeLine(line);
  }
}

void Protocol::handleExtended(char* line) {
  char* p = line;
  char cmd[16];
  size_t n = 0;
  while (*p != '\0' && *p != ' ' && n + 1 < sizeof(cmd)) {
    cmd[n++] = upperChar(*p++);
  }
  cmd[n] = '\0';
  while (*p == ' ') {
    ++p;
  }
  char* args = p;

  if (ieq(cmd, "XID")) {
    sendIdJson();
    return;
  }
  if (ieq(cmd, "XSTATUS")) {
    sendStatusJson();
    return;
  }
  if (ieq(cmd, "XCONFIG")) {
    sendConfigJson();
    return;
  }
  if (ieq(cmd, "XSET")) {
    char* eq = strchr(args, '=');
    if (eq == nullptr) {
      sendJsonError("missing_equals");
      return;
    }
    *eq = '\0';
    char* key = trim(args);
    char* value = trim(eq + 1);
    if (*key == '\0') {
      sendJsonError("missing_key");
      return;
    }
    if (strlen(key) >= 32) {
      sendJsonError("key_too_long");
      return;
    }
    char lower[32];
    copyLower(lower, sizeof(lower), key);

    char err[48] = {0};
    if (!host_.commApplySetting(lower, value, err, sizeof(err))) {
      sendJsonError(err[0] != '\0' ? err : "invalid_value");
      return;
    }
    jsonReset();
    jsonAdd("{\"ok\":true,\"key\":\"%s\",\"saved\":false}", lower);
    replyJson();
    return;
  }
  if (ieq(cmd, "XSAVE")) {
    char err[48] = {0};
    if (!host_.commSaveSettings(err, sizeof(err))) {
      sendJsonError(err[0] != '\0' ? err : "save_failed");
      return;
    }
    jsonReset();
    jsonAdd("{\"ok\":true}");
    replyJson();
    return;
  }
  if (ieq(cmd, "XRESET")) {
    char err[48] = {0};
    if (!host_.commFactoryReset(err, sizeof(err))) {
      sendJsonError(err[0] != '\0' ? err : "reset_failed");
      return;
    }
    jsonReset();
    jsonAdd("{\"ok\":true,\"reset\":true}");
    replyJson();
    return;
  }
  if (ieq(cmd, "XSHUTDOWN")) {
    const char* end = nullptr;
    uint32_t seconds = 0;
    if (!parseUnsigned(args, seconds, &end)) {
      sendJsonError("bad_seconds");
      return;
    }
    while (*end == ' ') {
      ++end;
    }
    uint32_t restore_s = 0;
    if (*end != '\0') {
      if (!parseUnsigned(end, restore_s, &end)) {
        sendJsonError("bad_restore");
        return;
      }
      while (*end == ' ') {
        ++end;
      }
      if (*end != '\0') {
        sendJsonError("trailing_args");
        return;
      }
    }
    if (seconds > 86400ul || restore_s > 604800ul) {
      sendJsonError("time_out_of_range");
      return;
    }
    char err[48] = {0};
    if (!host_.commRequestShutdown(seconds, restore_s, err, sizeof(err))) {
      sendJsonError(err[0] != '\0' ? err : "shutdown_refused");
      return;
    }
    jsonReset();
    jsonAdd("{\"ok\":true,\"shutdown_in_s\":%lu,\"restore_in_s\":%lu}",
            (unsigned long)seconds, (unsigned long)restore_s);
    replyJson();
    return;
  }
  if (ieq(cmd, "XCANCEL")) {
    host_.commCancelShutdown();
    jsonReset();
    jsonAdd("{\"ok\":true}");
    replyJson();
    return;
  }
  if (ieq(cmd, "XTEST")) {
    char err[48] = {0};
    uint32_t seconds = 0;
    bool until_low = false;
    if (ieq(args, "low")) {
      until_low = true;
    } else {
      const char* end = nullptr;
      uint32_t value = 0;
      if (!parseUnsigned(args, value, &end) || *end != '\0' || value < 1 || value > 3600) {
        sendJsonError("bad_test_duration");
        return;
      }
      seconds = value;
    }
    if (!host_.commRequestTest(seconds, until_low, err, sizeof(err))) {
      sendJsonError(err[0] != '\0' ? err : "test_refused");
      return;
    }
    jsonReset();
    jsonAdd("{\"ok\":true}");
    replyJson();
    return;
  }
  if (ieq(cmd, "XBEEP")) {
    host_.commToggleBeeper();
    jsonReset();
    jsonAdd("{\"ok\":true,\"beeper\":%s}",
            host_.commSettings().beeper_enabled ? "true" : "false");
    replyJson();
    return;
  }
  if (ieq(cmd, "XHELP")) {
    jsonReset();
    jsonAdd("{\"ok\":true,\"cmds\":[\"XID\",\"XSTATUS\",\"XCONFIG\","
            "\"XSET key=value\",\"XSAVE\",\"XRESET\",\"XSHUTDOWN sec [restore]\","
            "\"XCANCEL\",\"XTEST sec|low\",\"XBEEP\"]}");
    replyJson();
    return;
  }

  sendJsonError("unknown_command");
}

void Protocol::sendIdJson() {
  jsonReset();
  jsonAdd("{\"ok\":true,\"device\":\"%s\",\"mfr\":\"%s\",\"fw\":\"%s\","
          "\"proto\":%u,\"board\":\"LCKFB-DKX-STM32F103C8T6\","
          "\"transport\":\"USB CDC\"}",
          UPS_DEVICE_NAME, UPS_MANUFACTURER, UPS_FW_VERSION,
          (unsigned)UPS_PROTO_VERSION);
  replyJson();
}

void Protocol::sendStatusJson() {
  ups::Telemetry t;
  host_.commGetTelemetry(t);
  const bool ac = ups::telemetryFlag(t, ups::TF_AC_PRESENT);
  const bool output = ups::telemetryFlag(t, ups::TF_OUTPUT_ON);
  const bool charging = ups::telemetryFlag(t, ups::TF_CHARGING);
  const bool done = ups::telemetryFlag(t, ups::TF_CHARGE_DONE);
  const bool battery = ups::telemetryFlag(t, ups::TF_BATTERY_PRESENT);
  const bool test = ups::telemetryFlag(t, ups::TF_TEST_ACTIVE);
  const bool shutdown = ups::telemetryFlag(t, ups::TF_SHUTDOWN_PENDING);
  const bool beeper = ups::telemetryFlag(t, ups::TF_BEEPER_ENABLED);

  jsonReset();
  jsonAdd("{\"ok\":true,\"device\":\"%s\",\"fw\":\"%s\",\"proto\":%u,",
          UPS_DEVICE_NAME, UPS_FW_VERSION, (unsigned)UPS_PROTO_VERSION);
  jsonAdd("\"uptime_s\":%lu,\"state\":\"%s\",\"state_code\":%u,",
          (unsigned long)t.uptime_s, stateName(t.state), (unsigned)t.state);
  jsonAdd("\"ac\":%s,\"output\":%s,\"charging\":%s,\"charge_done\":%s,",
          ac ? "true" : "false", output ? "true" : "false",
          charging ? "true" : "false", done ? "true" : "false");
  jsonAdd("\"battery_present\":%s,\"test\":%s,\"shutdown_pending\":%s,\"beeper\":%s,",
          battery ? "true" : "false", test ? "true" : "false",
          shutdown ? "true" : "false", beeper ? "true" : "false");
  jsonAdd("\"vin_mv\":%ld,\"vin_fault_mv\":%ld,\"vout_mv\":%ld,\"vbat_mv\":%ld,",
          (long)t.vin_mv, (long)t.vin_fault_mv, (long)t.vout_mv, (long)t.vbat_mv);
  jsonAdd("\"ibat_ma\":%ld,\"iout_ma\":%ld,\"temp_c_x10\":%d,",
          (long)t.ibat_ma, (long)t.iout_ma, (int)t.temp_c_x10);
  jsonAdd("\"soc_x10\":%u,\"runtime_s\":%lu,\"load_pct\":%u,",
          (unsigned)t.soc_x10, (unsigned long)t.runtime_s, (unsigned)t.load_pct);
  jsonAdd("\"faults\":%u,\"warnings\":%u,\"low\":%s,\"critical\":%s,",
          (unsigned)t.faults, (unsigned)t.warnings,
          t.battery_low ? "true" : "false", t.critical_battery ? "true" : "false");
  jsonAdd("\"ac_fail_count\":%lu,\"shutdown_remaining_s\":%lu,\"restore_remaining_s\":%lu,",
          (unsigned long)t.ac_fail_count, (unsigned long)t.shutdown_remaining_s,
          (unsigned long)t.restore_remaining_s);
  jsonAdd("\"ina_ok\":%s,\"ntc_ok\":%s,\"output_sensor\":%s,\"config_dirty\":%s}",
          t.ina_ok ? "true" : "false", t.ntc_ok ? "true" : "false",
          t.output_sensor_ok ? "true" : "false", t.config_dirty ? "true" : "false");
  replyJson();
}

void Protocol::sendConfigJson() {
  const Settings& s = host_.commSettings();
  ups::Telemetry t;
  host_.commGetTelemetry(t);

  jsonReset();
  jsonAdd("{\"ok\":true,\"fw\":\"%s\",\"dirty\":%s,", UPS_FW_VERSION,
          t.config_dirty ? "true" : "false");
  jsonAdd("\"cells\":%u,\"capacity_mah\":%u,\"low_pct\":%u,\"critical_pct\":%u,",
          (unsigned)s.cells, (unsigned)s.capacity_mAh,
          (unsigned)s.low_pct, (unsigned)s.critical_pct);
  jsonAdd("\"cutoff_grace_s\":%u,\"ac_restore_s\":%u,",
          (unsigned)s.cutoff_grace_s, (unsigned)s.ac_restore_s);
  jsonAdd("\"cell_nominal_mv\":%u,\"cell_full_mv\":%u,\"cell_empty_mv\":%u,",
          (unsigned)s.cell_nominal_mv, (unsigned)s.cell_full_mv,
          (unsigned)s.cell_empty_mv);
  jsonAdd("\"cell_low_mv\":%u,\"cell_critical_mv\":%u,\"cell_ov_mv\":%u,",
          (unsigned)s.cell_low_mv, (unsigned)s.cell_critical_mv,
          (unsigned)s.cell_ov_mv);
  jsonAdd("\"vbat_scale\":%d,\"vbat_offset\":%d,\"vin_scale\":%d,\"vin_offset\":%d,",
          (int)s.vbat_scale_permille, (int)s.vbat_offset_mv,
          (int)s.vin_scale_permille, (int)s.vin_offset_mv);
  jsonAdd("\"vout_scale\":%d,\"vout_offset\":%d,",
          (int)s.vout_scale_permille, (int)s.vout_offset_mv);
  jsonAdd("\"shunt_mohm\":%u,\"max_charge_ma\":%u,\"max_discharge_ma\":%u,",
          (unsigned)s.shunt_mohm, (unsigned)s.max_charge_ma,
          (unsigned)s.max_discharge_ma);
  jsonAdd("\"max_load_ma\":%u,\"assumed_load_ma\":%u,",
          (unsigned)s.max_load_ma, (unsigned)s.assumed_load_ma);
  jsonAdd("\"vin_present_mv\":%u,\"vin_nominal_mv\":%u,\"vin_max_mv\":%u,",
          (unsigned)s.vin_present_mv, (unsigned)s.vin_nominal_mv,
          (unsigned)s.vin_max_mv);
  jsonAdd("\"vout_nominal_mv\":%u,\"vout_uv_mv\":%u,\"vout_ov_mv\":%u,",
          (unsigned)s.vout_nominal_mv, (unsigned)s.vout_uv_mv,
          (unsigned)s.vout_ov_mv);
  jsonAdd("\"charge_max_temp\":%d,\"discharge_min_temp\":%d,"
          "\"discharge_max_temp\":%d,\"temp_fault\":%d,\"temp_offset\":%d,",
          (int)s.charge_max_temp_c_x10, (int)s.discharge_min_temp_c_x10,
          (int)s.discharge_max_temp_c_x10, (int)s.temp_fault_c_x10,
          (int)s.temp_offset_c_x10);
  jsonAdd("\"nominal_freq_x10\":%u,\"beeper_enabled\":%u,\"ntc_enabled\":%u,"
          "\"ina_invert\":%u,\"auto_recover\":%u}",
          (unsigned)s.nominal_freq_x10, (unsigned)s.beeper_enabled,
          (unsigned)s.ntc_enabled, (unsigned)s.ina_invert,
          (unsigned)s.auto_recover);
  replyJson();
}

void Protocol::writeRaw(const char* text, size_t len) {
  io_.write(reinterpret_cast<const uint8_t*>(text), len);
}

void Protocol::writeLine(const char* text) {
  io_.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
  const uint8_t cr = '\r';
  io_.write(&cr, 1);
}

void Protocol::sendJsonError(const char* error) {
  jsonReset();
  jsonAdd("{\"ok\":false,\"error\":\"%s\"}", error != nullptr ? error : "error");
  replyJson();
}

void Protocol::jsonReset() {
  json_len_ = 0;
  json_overflow_ = false;
  json_[0] = '\0';
}

void Protocol::jsonAdd(const char* fmt, ...) {
  if (json_overflow_ || json_len_ >= sizeof(json_) - 1) {
    json_overflow_ = true;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const size_t remaining = sizeof(json_) - json_len_;
  int n = vsnprintf(json_ + json_len_, remaining, fmt, ap);
  va_end(ap);
  if (n < 0) {
    json_overflow_ = true;
    return;
  }
  if (static_cast<size_t>(n) >= remaining) {
    json_overflow_ = true;
    json_len_ = sizeof(json_) - 1;
    return;
  }
  json_len_ += static_cast<size_t>(n);
}

void Protocol::replyJson() {
  if (json_overflow_) {
    jsonReset();
    jsonAdd("{\"ok\":false,\"error\":\"response_too_large\"}");
  }
  jsonAdd("\r\n");
  writeRaw(json_, json_len_);
}
