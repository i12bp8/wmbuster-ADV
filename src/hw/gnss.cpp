// wM-Buster ADV — GNSS receiver.
// GPL-3.0
#include "gnss.h"

#include <Arduino.h>
#include <TinyGPS++.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"

namespace wmb {

static TinyGPSPlus s_gps;
static HardwareSerial s_uart(2);
static bool s_running = false;
static uint32_t s_baud = 0;
static uint32_t s_baud_ms = 0;
static uint32_t s_passed_at_switch = 0;
static uint32_t s_time_set_ms = 0;

static void open_uart(uint32_t baud) {
    if (s_running) s_uart.end();
    s_uart.begin(baud, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);
    s_running = true;
    s_baud = baud;
    s_baud_ms = millis();
    s_passed_at_switch = s_gps.passedChecksum();
}

void gnss_begin() { open_uart(GNSS_BAUD); }

void gnss_end() {
    if (s_running) s_uart.end();
    s_running = false;
}

static bool days_from_civil_ok(int y) { return y >= 2024 && y < 2100; }

static void maybe_set_clock() {
    if (!s_gps.date.isValid() || !s_gps.time.isValid() || s_gps.time.age() > 1500) return;
    if (!days_from_civil_ok(s_gps.date.year())) return;
    uint32_t now = millis();
    if (s_time_set_ms && now - s_time_set_ms < 600000) return;  // every 10 minutes at most
    struct tm tmv = {};
    tmv.tm_year = s_gps.date.year() - 1900;
    tmv.tm_mon = s_gps.date.month() - 1;
    tmv.tm_mday = s_gps.date.day();
    tmv.tm_hour = s_gps.time.hour();
    tmv.tm_min = s_gps.time.minute();
    tmv.tm_sec = s_gps.time.second();
    // mktime() works in local time; the device runs with TZ=UTC.
    time_t t = mktime(&tmv);
    if (t <= 0) return;
    time_t cur = time(nullptr);
    if (cur > 1600000000 && labs((long)(cur - t)) < 2) {
        s_time_set_ms = now;
        return;
    }
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    s_time_set_ms = now;
    Serial.printf("[GNSS] clock set to %04d-%02d-%02d %02d:%02d:%02d UTC\n", s_gps.date.year(), s_gps.date.month(),
                  s_gps.date.day(), s_gps.time.hour(), s_gps.time.minute(), s_gps.time.second());
}

void gnss_loop() {
    if (!s_running) return;
    int budget = 512;
    while (budget-- > 0 && s_uart.available()) s_gps.encode((char)s_uart.read());
    // Auto baud: no valid sentence within 3 s -> try the other common rate.
    if (s_gps.passedChecksum() == s_passed_at_switch && millis() - s_baud_ms > 3000) {
        open_uart(s_baud == 115200 ? 9600 : 115200);
    }
    maybe_set_clock();
}

bool gnss_position(double* lat, double* lon) {
    if (!s_running || !s_gps.location.isValid() || s_gps.location.age() > 5000) return false;
    *lat = s_gps.location.lat();
    *lon = s_gps.location.lng();
    return true;
}

void gnss_info(GnssInfo* o) {
    o->present = s_running && s_gps.passedChecksum() > s_passed_at_switch;
    o->fix = gnss_position(&o->lat, &o->lon);
    if (!o->fix) o->lat = o->lon = 0;
    o->alt_m = s_gps.altitude.isValid() ? (float)s_gps.altitude.meters() : 0;
    o->hdop = s_gps.hdop.isValid() ? (float)s_gps.hdop.hdop() : 0;
    o->sats = s_gps.satellites.isValid() ? (uint8_t)s_gps.satellites.value() : 0;
    o->baud = s_baud;
}

} // namespace wmb
