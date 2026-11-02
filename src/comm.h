#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"
#include "telemetry.h"

class CommHost {
 public:
  virtual ~CommHost() {}

  virtual void commGetTelemetry(ups::Telemetry& out) = 0;
  virtual const Settings& commSettings() const = 0;

  virtual bool commApplySetting(const char* key, const char* value, char* err, size_t cap) = 0;
  virtual bool commSaveSettings(char* err, size_t cap) = 0;
  virtual bool commFactoryReset(char* err, size_t cap) = 0;

  virtual bool commRequestShutdown(uint32_t after_s, uint32_t restore_s,
                                   char* err, size_t cap) = 0;
  virtual void commCancelShutdown() = 0;
  virtual bool commRequestTest(uint32_t seconds, bool until_low,
                               char* err, size_t cap) = 0;
  virtual void commCancelTest() = 0;
  virtual void commToggleBeeper() = 0;
};

/*
 * Line protocol over USB CDC (Type-C).
 *
 * Megatec/Q1-compatible commands (NUT nutdrv_qx / blazer):
 *   Q1, F, I, Q, T, TL, T<minutes>, CT, C, S<minutes>[R<minutes>]
 *
 * Extended JSON commands for the supplied host tool:
 *   XID, XSTATUS, XCONFIG, XSET key=value, XSAVE, XRESET,
 *   XSHUTDOWN <seconds> [restore_seconds], XCANCEL, XTEST <seconds|low>,
 *   XBEEP, XHELP
 */
class Protocol {
 public:
  Protocol(Stream& io, CommHost& host);

  void poll();

 private:
  void consume(char c);
  void handleLine();
  void handleMegatec(char* line);
  void handleExtended(char* line);
  void handleShutdownCommand(const char* line);
  void handleTestCommand(const char* line, const char* args);

  void writeRaw(const char* text, size_t len);
  void writeLine(const char* text);
  void sendJsonError(const char* error);
  void jsonReset();
  void jsonAdd(const char* fmt, ...);
  void replyJson();

  void sendIdJson();
  void sendStatusJson();
  void sendConfigJson();

  Stream& io_;
  CommHost& host_;

  char line_[128];
  uint8_t line_len_;
  bool line_overflow_;

  char json_[1400];
  size_t json_len_;
  bool json_overflow_;
};
