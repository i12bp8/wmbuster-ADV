// wM-Buster ADV — application core: radio captures -> frames -> decoded
// telegrams -> meter table, feed and outputs (serial, SD, MQTT, ntfy, UI).
// Runs in the Arduino loop task; the radio has its own task.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/engine.h"
#include "meters.h"

namespace wmb {

struct AppStats {
    uint32_t captures;     // raw captures from the radio
    uint32_t frames;       // captures with valid DLL CRCs
    uint32_t crc_errors;
    uint32_t bad_coding;
    uint32_t truncated;
    uint32_t decoded;      // telegrams decoded into fields
    uint32_t encrypted;    // no key / wrong key
    uint32_t undecoded;    // compact without format, manufacturer payload, ...
    uint32_t c1, t1, s1;
    uint32_t per_min;      // telegrams during the last minute
};

extern AppStats g_app;

// Last telegram, for UI notifications.
struct AppEvent {
    uint32_t seq;          // increments for every telegram
    uint32_t ms;
    char     id[9];
    bool     is_new;
    bool     starred;
    bool     alarm;
    bool     decoded;
};

void app_begin();
void app_loop();

const AppEvent& app_last_event();

// Radio control (settings applied at runtime).
void app_apply_radio_band();
const char* app_band_label();       // "C1/T1", "S1" or "C1/T1+S1"

// Shared decoder for views: decode the last frame of a meter again (with the
// configured key/driver). Returns nullptr when the meter has no frame.
const Decoder* app_redecode(const Meter* m);

// Analyzer: decode hex (radio capture, frame with or without CRCs, wired
// M-Bus); key_hex and driver may be empty. The meter table is not touched.
const Decoder* app_analyze(const char* hex, const char* key_hex, const char* driver, bool* frame_ok);

// Meter configuration changed (key/driver/name): refresh derived state.
void app_meter_config_changed(const char* id);

// Current UNIX time or 0 when unknown (NTP / GNSS).
uint32_t app_unix_time();

} // namespace wmb
