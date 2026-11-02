#include "config_store.h"

#include <Arduino.h>

bool ConfigStore::begin() {
  const bool valid = nvm_.begin();
  dirty_ = false;
  dirty_since_ms_ = 0;
  return valid;
}

bool ConfigStore::apply(const char* key, const char* value, char* err, size_t cap) {
  Settings candidate = nvm_.settings();
  if (!settingsApply(candidate, key, value, err, cap)) {
    return false;
  }
  if (!settingsValidate(candidate, err, cap)) {
    return false;
  }
  nvm_.settingsMutable() = candidate;
  dirty_ = true;
  dirty_since_ms_ = millis();
  return true;
}

bool ConfigStore::save() {
  if (!nvm_.save()) {
    return false;
  }
  dirty_ = false;
  return true;
}

bool ConfigStore::factoryReset() {
  settingsDefaults(nvm_.settingsMutable());
  runtimeStateDefaults(nvm_.stateMutable());
  dirty_ = true;
  return save();
}

void ConfigStore::tick(uint32_t now_ms) {
  if (dirty_ && (now_ms - dirty_since_ms_ >= 60000)) {
    save();
  }
}
