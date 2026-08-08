#pragma once
// Minimal logging shim. The X3 firmware mirrors serial into an RTC ring buffer
// (log_buffer); on the TRMNL X we keep it simple and log straight to the USB
// CDC serial port. `Log` is used by the ported crypto/wifi modules.
#include <Arduino.h>

#define Log Serial
