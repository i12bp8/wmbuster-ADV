// wM-Buster ADV — USB serial console: telegram output in several formats and
// a small command line (type "help").
//
// Output formats (setting "serial"):
//   log       human readable lines
//   json      one wmbusmeters json object per telegram
//   rtlwmbus  rtl_wmbus lines; feed a PC wmbusmeters with
//             wmbusmeters --device=stdin:rtlwmbus < /dev/ttyACM0 ...
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/engine.h"
#include "radio/radio.h"

namespace wmb {

void console_begin();
void console_loop();
void console_telegram(const Frame& f, const Decoder& d, int16_t rssi, const char* name);
void console_capture_error(const RadioCapture& c, CaptureStatus st);

// Shared formatters (also used by SD logging, MQTT and the web API).
// "T1;1;1;2024-05-01 12:00:00.000;-68;-68;12345678;0x2E44..."
size_t format_rtlwmbus(const Frame& f, int16_t rssi, char* out, size_t cap);
// wmbusmeters json with rssi_dbm (and lat/lon when pos is given).
size_t format_json(const Decoder& d, int16_t rssi, const char* name, const double* pos, char* out, size_t cap);
// Current time as wmbusmeters prints it ("2024-05-01T12:00:00Z"), or empty.
void format_timestamp(char* out, size_t cap);

} // namespace wmb
