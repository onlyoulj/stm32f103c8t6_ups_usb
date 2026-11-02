#pragma once

// Minimal Arduino Stream stub used only by the host-side protocol test.
#include <stddef.h>
#include <stdint.h>

class Stream {
 public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual size_t write(const uint8_t* data, size_t len) = 0;

  size_t write(uint8_t data) { return write(&data, 1); }
  size_t write(const char* data, size_t len) {
    return write(reinterpret_cast<const uint8_t*>(data), len);
  }
};
