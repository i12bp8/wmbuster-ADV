// wM-Buster ADV — SD card logging.
// GPL-3.0
#include "sdlog.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

#include "config.h"
#include "console.h"
#include "settings.h"
#include "scratch.h"
#include "radio/radio.h"

namespace wmb {

static bool s_card = false;
static uint32_t s_lines = 0;
static uint32_t s_dropped = 0;
static uint32_t s_last_flush = 0;
static uint32_t s_last_check = 0;

enum { F_RTL = 0, F_JSON = 1, F_WARD = 2, F_COUNT = 3 };
static const char* const PATHS[F_COUNT] = { "/wmbuster/telegrams.rtl", "/wmbuster/telegrams.jsonl",
                                            "/wmbuster/wardrive.csv" };
static File s_files[F_COUNT];
static bool s_dirty[F_COUNT];

// Pending lines: [tag][text...]\n records in a byte ring.
static char s_ring[4096];
static size_t s_head = 0, s_tail = 0, s_used = 0;

static bool ring_push(uint8_t tag, const char* text, size_t n) {
    if (n + 2 > sizeof(s_ring) - s_used) return false;
    auto put = [](char c) {
        s_ring[s_head] = c;
        s_head = (s_head + 1) % sizeof(s_ring);
        s_used++;
    };
    put((char)('0' + tag));
    for (size_t i = 0; i < n; ++i) put(text[i]);
    put('\n');
    return true;
}

// Pops one record into buf (without the newline); returns its tag or -1.
static int ring_pop(char* buf, size_t cap, size_t* len) {
    if (!s_used) return -1;
    int tag = s_ring[s_tail] - '0';
    s_tail = (s_tail + 1) % sizeof(s_ring);
    s_used--;
    size_t n = 0;
    while (s_used) {
        char c = s_ring[s_tail];
        s_tail = (s_tail + 1) % sizeof(s_ring);
        s_used--;
        if (c == '\n') break;
        if (n + 1 < cap) buf[n++] = c;
    }
    *len = n;
    return tag;
}

bool sdlog_begin() {
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
    s_card = SD.begin(PIN_SD_CS, SPI, 20000000);
    if (s_card) {
        SD.mkdir("/wmbuster");
        Serial.printf("[SD] card %llu MB\n", (unsigned long long)(SD.cardSize() / (1024 * 1024)));
    } else {
        Serial.println("[SD] no card");
    }
    return s_card;
}

bool sdlog_card_ok() { return s_card; }

uint64_t sdlog_card_free() {
    if (!s_card) return 0;
    uint64_t total = SD.totalBytes(), used = SD.usedBytes();
    return total > used ? total - used : 0;
}

uint32_t sdlog_lines() { return s_lines; }
uint32_t sdlog_dropped() { return s_dropped; }

static void close_all() {
    for (int i = 0; i < F_COUNT; ++i) {
        if (s_files[i]) s_files[i].close();
        s_dirty[i] = false;
    }
}

static bool write_line(int tag, const char* text, size_t n) {
    if (tag < 0 || tag >= F_COUNT) return true;
    File& f = s_files[tag];
    if (!f) {
        bool fresh = !SD.exists(PATHS[tag]);
        f = SD.open(PATHS[tag], FILE_APPEND);
        if (!f) return false;
        if (fresh && tag == F_WARD) f.println("time,id,mfct,version,type,driver,mode,rssi_dbm,lat,lon,value");
    }
    size_t w = f.write((const uint8_t*)text, n);
    w += f.write((const uint8_t*)"\n", 1);
    s_dirty[tag] = true;
    return w == n + 1;
}

void sdlog_loop() {
    uint32_t now = millis();
    if (!s_card) {
        s_head = s_tail = s_used = 0;
        return;
    }
    char* line = scratch();
    // Write while the radio is idle (the card shares its SPI bus).
    for (int budget = 0; budget < 6 && s_used && !radio_busy(); ++budget) {
        size_t n = 0;
        int tag = ring_pop(line, SCRATCH_LEN, &n);
        if (!write_line(tag, line, n)) {
            // Card removed or full: stop logging until the next check.
            s_card = false;
            close_all();
            Serial.println("[SD] write failed, card removed?");
            return;
        }
        s_lines++;
    }
    if (now - s_last_flush > 5000 && !radio_busy()) {
        s_last_flush = now;
        for (int i = 0; i < F_COUNT; ++i)
            if (s_dirty[i] && s_files[i]) {
                s_files[i].flush();
                s_dirty[i] = false;
            }
    }
    (void)s_last_check;
}

static void iso_time(char* out, size_t cap) {
    time_t t = time(nullptr);
    if (t < 1600000000) {
        snprintf(out, cap, "uptime+%lus", (unsigned long)(millis() / 1000));
        return;
    }
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void sdlog_telegram(const Frame& f, const Decoder& d, int16_t rssi, const double* pos) {
    if (!g_cfg.sd_log || !s_card) return;
    char* buf = scratch();
    const size_t cap = SCRATCH_LEN;
    const DecodeResult& r = d.res;
    size_t n = format_rtlwmbus(f, rssi, buf, cap);
    if (n && !ring_push(F_RTL, buf, n)) s_dropped++;
    if (r.status == DecodeStatus::Ok) {
        n = format_json(d, rssi, nullptr, pos, buf, cap);
        if (n && !ring_push(F_JSON, buf, n)) s_dropped++;
    }
    if (pos) {
        char ts[32], sum[40] = "";
        iso_time(ts, sizeof(ts));
        const OutField* first = nullptr;
        for (int i = 0; i < r.num_fields && !first; ++i)
            if (!r.fields[i].hidden && !r.fields[i].is_text) first = &r.fields[i];
        if (first) field_format(first, sum, sizeof(sum));
        int w = snprintf(buf, cap, "%s,%s,%s,%02X,%02X,%s,%s,%d,%.6f,%.6f,%s", ts, r.id, r.mfct, r.version, r.type,
                         r.driver ? r.driver->name : "", link_mode_name(f.mode), rssi, pos[0], pos[1], sum);
        if (w < 0 || (size_t)w >= cap || !ring_push(F_WARD, buf, (size_t)w)) s_dropped++;
    }
}

} // namespace wmb
