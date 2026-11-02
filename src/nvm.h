#pragma once

#include <stdint.h>

#include "settings.h"

struct RuntimeState {
  uint32_t magic;
  uint16_t version;
  uint16_t soc_x10;
  uint32_t ac_fail_count;
  uint32_t cycle_count;
  uint32_t total_runtime_s;
  uint32_t last_fault;
  uint16_t reserved[3];
};

void runtimeStateDefaults(RuntimeState& s);

/*
 * Power-fail-safe two-page NVM.
 *
 * The last two 1 KiB flash pages (0x0800F800 and 0x0800FC00) are reserved by
 * ldscript/stm32f103c8_nvm.ld.  Each save erases and rewrites the page that is
 * NOT currently active, then writes the magic word last.  A power loss during
 * the write leaves the previous page intact.  Both pages carry a CRC32.
 */
class Nvm {
 public:
  bool begin();

  bool hadValidPage() const { return nvm_valid_; }
  bool settingsValid() const { return settings_valid_; }
  uint16_t sequence() const { return sequence_; }

  const Settings& settings() const { return settings_; }
  Settings& settingsMutable() { return settings_; }
  const RuntimeState& state() const { return state_; }
  RuntimeState& stateMutable() { return state_; }

  bool save();

 private:
  bool readPage(uint32_t address, Settings& s, RuntimeState& st, uint16_t& seq);
  bool programPage(uint32_t address, const Settings& s, const RuntimeState& st, uint16_t seq);

  Settings settings_;
  RuntimeState state_;
  int8_t current_page_;
  uint16_t sequence_;
  bool nvm_valid_;
  bool settings_valid_;
};
