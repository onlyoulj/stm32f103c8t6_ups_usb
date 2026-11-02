#include "nvm.h"

#include <Arduino.h>
#include <stm32f1xx_hal.h>
#include <stm32f1xx_hal_flash.h>
#include <stm32f1xx_hal_flash_ex.h>
#include <string.h>

namespace {

constexpr uint32_t NVM_PAGE_A = 0x0800F800u;  // page 62 of 64 KiB
constexpr uint32_t NVM_PAGE_B = 0x0800FC00u;  // page 63 of 64 KiB
constexpr uint32_t NVM_MAGIC = 0x4E564D31u;   // "NVM1"
constexpr uint16_t NVM_VERSION = 1;
constexpr uint32_t STATE_MAGIC = 0x53544131u; // "STA1"
constexpr uint16_t STATE_VERSION = 1;

struct Header {
  uint32_t magic;
  uint16_t version;
  uint16_t sequence;
  uint16_t payload_len;
  uint16_t reserved;
  uint32_t payload_crc;
};

struct Payload {
  Settings settings;
  RuntimeState state;
};

static_assert(sizeof(Header) % 2 == 0, "header must be halfword aligned");
static_assert(sizeof(Payload) % 2 == 0, "payload must be halfword aligned");

uint32_t crc32(const void* data, size_t len) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
  }
  return ~crc;
}

class IrqLock {
 public:
  IrqLock() { noInterrupts(); }
  ~IrqLock() { interrupts(); }
};

}  // namespace

void runtimeStateDefaults(RuntimeState& s) {
  memset(&s, 0, sizeof(s));
  s.magic = STATE_MAGIC;
  s.version = STATE_VERSION;
  s.soc_x10 = 500;
}

bool Nvm::begin() {
  settingsDefaults(settings_);
  runtimeStateDefaults(state_);
  current_page_ = -1;
  sequence_ = 0;
  nvm_valid_ = false;
  settings_valid_ = false;

  Settings a;
  Settings b;
  RuntimeState sa;
  RuntimeState sb;
  uint16_t seq_a = 0;
  uint16_t seq_b = 0;
  const bool ok_a = readPage(NVM_PAGE_A, a, sa, seq_a);
  const bool ok_b = readPage(NVM_PAGE_B, b, sb, seq_b);

  const Settings* chosen = nullptr;
  const RuntimeState* chosen_state = nullptr;
  uint16_t chosen_seq = 0;
  int8_t chosen_page = -1;

  if (ok_a && ok_b) {
    // Signed 16-bit difference keeps "newest" correct across sequence wrap.
    const int16_t diff = static_cast<int16_t>(seq_a - seq_b);
    if (diff < 0) {
      chosen = &b;
      chosen_state = &sb;
      chosen_seq = seq_b;
      chosen_page = 1;
    } else {
      chosen = &a;
      chosen_state = &sa;
      chosen_seq = seq_a;
      chosen_page = 0;
    }
  } else if (ok_a) {
    chosen = &a;
    chosen_state = &sa;
    chosen_seq = seq_a;
    chosen_page = 0;
  } else if (ok_b) {
    chosen = &b;
    chosen_state = &sb;
    chosen_seq = seq_b;
    chosen_page = 1;
  }

  if (chosen == nullptr) {
    return false;
  }

  nvm_valid_ = true;
  current_page_ = chosen_page;
  sequence_ = chosen_seq;

  Settings temp = *chosen;
  char err[48] = {0};
  if (temp.magic == SETTINGS_MAGIC && temp.version == SETTINGS_VERSION &&
      temp.crc == settingsCrc(temp) && settingsValidate(temp, err, sizeof(err))) {
    settings_ = temp;
    settings_valid_ = true;
  } else {
    settingsDefaults(settings_);
    settings_valid_ = false;
  }

  RuntimeState st = *chosen_state;
  if (st.magic == STATE_MAGIC && st.version == STATE_VERSION && st.soc_x10 <= 1000) {
    state_ = st;
  } else {
    runtimeStateDefaults(state_);
  }
  return true;
}

bool Nvm::readPage(uint32_t address, Settings& s, RuntimeState& st, uint16_t& seq) {
  Header h;
  memcpy(&h, reinterpret_cast<const void*>(address), sizeof(h));
  if (h.magic != NVM_MAGIC || h.version != NVM_VERSION ||
      h.payload_len != sizeof(Payload)) {
    return false;
  }

  Payload payload;
  memcpy(&payload, reinterpret_cast<const void*>(address + sizeof(Header)), sizeof(payload));
  if (h.payload_crc != crc32(&payload, sizeof(payload))) {
    return false;
  }

  s = payload.settings;
  st = payload.state;
  seq = h.sequence;
  return true;
}

bool Nvm::save() {
  char err[48] = {0};
  if (!settingsValidate(settings_, err, sizeof(err))) {
    return false;
  }
  settingsFinalize(settings_);
  state_.magic = STATE_MAGIC;
  state_.version = STATE_VERSION;
  if (state_.soc_x10 > 1000) {
    state_.soc_x10 = 1000;
  }

  const uint32_t target = (current_page_ == 0) ? NVM_PAGE_B : NVM_PAGE_A;
  uint16_t next_seq = sequence_ + 1;
  if (next_seq == 0) {
    next_seq = 1;
  }
  if (!programPage(target, settings_, state_, next_seq)) {
    return false;
  }

  current_page_ = (current_page_ == 0) ? 1 : 0;
  sequence_ = next_seq;
  nvm_valid_ = true;
  settings_valid_ = true;
  return true;
}

bool Nvm::programPage(uint32_t address, const Settings& s, const RuntimeState& st, uint16_t seq) {
  Payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.settings = s;
  payload.state = st;

  Header header;
  memset(&header, 0, sizeof(header));
  header.magic = 0xFFFFFFFFu;  // written last
  header.version = NVM_VERSION;
  header.sequence = seq;
  header.payload_len = sizeof(Payload);
  header.payload_crc = crc32(&payload, sizeof(payload));

  uint8_t buffer[sizeof(Header) + sizeof(Payload)];
  memcpy(buffer, &header, sizeof(header));
  memcpy(buffer + sizeof(Header), &payload, sizeof(payload));
  const size_t total = sizeof(buffer);

  bool ok = false;
  {
    IrqLock lock;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

    FLASH_EraseInitTypeDef erase;
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_1;
    erase.PageAddress = address;
    erase.NbPages = 1;
    uint32_t page_error = 0;
    if (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK) {
      ok = true;
      for (size_t offset = 0; offset < total; offset += 2) {
        uint16_t half = static_cast<uint16_t>(buffer[offset]) |
                        (static_cast<uint16_t>(buffer[offset + 1]) << 8);
        if (half == 0xFFFFu) {
          continue;
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address + offset, half) != HAL_OK) {
          ok = false;
          break;
        }
      }

      if (ok) {
        // Commit marker: only now does the page become valid.
        const uint16_t magic_lo = static_cast<uint16_t>(NVM_MAGIC & 0xFFFFu);
        const uint16_t magic_hi = static_cast<uint16_t>(NVM_MAGIC >> 16);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address, magic_lo) != HAL_OK ||
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address + 2, magic_hi) != HAL_OK) {
          ok = false;
        }
      }
    }
    HAL_FLASH_Lock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);
  }

  if (!ok) {
    return false;
  }

  Settings verify_s;
  RuntimeState verify_st;
  uint16_t verify_seq = 0;
  return readPage(address, verify_s, verify_st, verify_seq) &&
         verify_seq == seq &&
         verify_s.magic == SETTINGS_MAGIC &&
         verify_s.crc == settingsCrc(verify_s);
}
