#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nvm.h"
#include "settings.h"

class ConfigStore {
 public:
  bool begin();

  const Settings& settings() const { return nvm_.settings(); }
  const RuntimeState& state() const { return nvm_.state(); }
  RuntimeState& stateMutable() { return nvm_.stateMutable(); }

  bool apply(const char* key, const char* value, char* err, size_t cap);
  bool save();
  bool factoryReset();
  void tick(uint32_t now_ms);

  bool dirty() const { return dirty_; }
  bool hadValidPage() const { return nvm_.hadValidPage(); }
  bool settingsValid() const { return nvm_.settingsValid(); }
  uint16_t nvmSequence() const { return nvm_.sequence(); }

 private:
  Nvm nvm_;
  bool dirty_ = false;
  uint32_t dirty_since_ms_ = 0;
};
