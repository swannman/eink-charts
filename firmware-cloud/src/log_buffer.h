#pragma once
// Mirror of Serial output into an RTC-backed ring buffer so we can render
// "logs since last full power-cycle" on a dedicated panel. Survives deep
// sleep (RTC slow memory), dies on EN/power-cycle — which is the right
// scope for "what's happened since I last booted this thing cold."
//
// Usage: replace `Serial.printf(...)` with `Log.printf(...)`. Log is a
// Print subclass that writes to Serial AND appends to the ring buffer.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace log_buffer {

constexpr size_t SIZE = 4096;

// Copy the buffer contents into `out` in chronological order, starting
// from the oldest retained byte. Returns the number of bytes written
// (<= SIZE). out must hold at least SIZE bytes.
size_t snapshot(uint8_t* out);

// Total number of valid bytes currently in the ring buffer.
size_t length();

}  // namespace log_buffer

class MirrorPrint : public Print {
 public:
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t len) override;
};

extern MirrorPrint Log;
