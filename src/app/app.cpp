// wM-Buster ADV — application core.
// GPL-3.0
#include "app.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "radio/radio.h"
#include "settings.h"
#include "scratch.h"
#include "sdlog.h"
#include "console.h"
#include "../hw/board.h"
#include "../hw/gnss.h"
#include "../net/mqtt.h"
#include "../net/ntfy.h"

namespace wmb {

AppStats g_app;

char* scratch() {
    static char buf[SCRATCH_LEN];
    return buf;
}

static Decoder s_dec;            // shared by the pipeline and the views (loop task only)
static RadioCapture s_cap;
static AppEvent s_event;
static uint32_t s_hop_ms = 0;
static uint16_t s_minute[60];    // telegrams per second, sliding window
static uint32_t s_minute_sec = 0;

const AppEvent& app_last_event() { return s_event; }

uint32_t app_unix_time() {
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
}

static RadioBand band_for_mode() {
    return g_cfg.band_mode == (uint8_t)BandMode::S ? RadioBand::S : RadioBand::CT;
}

void app_apply_radio_band() {
    s_hop_ms = millis();
    radio_set_band(band_for_mode());
}

const char* app_band_label() {
    if (g_cfg.band_mode == (uint8_t)BandMode::Hop) return "C1/T1+S1";
    return g_cfg.band_mode == (uint8_t)BandMode::S ? "S1" : "C1/T1";
}

void app_begin() {
    memset(&g_app, 0, sizeof(g_app));
    memset(&s_event, 0, sizeof(s_event));
    memset(s_minute, 0, sizeof(s_minute));
    s_hop_ms = millis();
}

static void count_minute() {
    uint32_t sec = millis() / 1000;
    if (sec != s_minute_sec) {
        uint32_t steps = sec - s_minute_sec;
        if (steps > 60) steps = 60;
        for (uint32_t i = 1; i <= steps; ++i) s_minute[(s_minute_sec + i) % 60] = 0;
        s_minute_sec = sec;
    }
    s_minute[sec % 60]++;
}

static void update_per_min() {
    uint32_t sec = millis() / 1000;
    if (sec != s_minute_sec) {
        uint32_t steps = sec - s_minute_sec;
        if (steps > 60) steps = 60;
        for (uint32_t i = 1; i <= steps; ++i) s_minute[(s_minute_sec + i) % 60] = 0;
        s_minute_sec = sec;
    }
    uint32_t sum = 0;
    for (uint16_t v : s_minute) sum += v;
    g_app.per_min = sum;
}

// DLL address of a wireless frame (LSB first at offset 4) as printed id.
static bool frame_dll_id(const Frame& f, char out[9]) {
    if (f.mode == LinkMode::MBus || f.len < 10) return false;
    snprintf(out, 9, "%02x%02x%02x%02x", f.data[7], f.data[6], f.data[5], f.data[4]);
    return true;
}

static DecodeOptions options_for(const MeterConf* mc) {
    DecodeOptions o;
    memset(&o, 0, sizeof(o));
    o.generic_fallback = true;
    o.now_unix = app_unix_time();
    if (mc) {
        if (mc->key_len) {
            o.key = mc->key;
            o.key_len = mc->key_len;
        }
        if (mc->driver[0]) o.forced_driver = driver_by_name(mc->driver);
    }
    return o;
}

// Decode with the configuration of the meter (looked up by the DLL address
// first, then by the decoded id, which differs when a TPL address is used).
static const MeterConf* decode_configured(const Frame& f, bool simulated) {
    char dll[9];
    const MeterConf* mc = frame_dll_id(f, dll) ? meterconf_find(dll) : nullptr;
    DecodeOptions o = options_for(mc);
    o.simulated = simulated;
    wmbus_decode(&s_dec, f, o);
    if (!mc && s_dec.res.id[0]) {
        const MeterConf* mc2 = meterconf_find(s_dec.res.id);
        if (mc2) {
            mc = mc2;
            o = options_for(mc);
            o.simulated = simulated;
            wmbus_decode(&s_dec, f, o);
        }
    }
    return mc;
}

static void notify_if_needed(const Meter* m, const MeterConf* mc, bool is_new, const char* prev_status) {
    if (!mc || !mc->starred) return;
    const char* label = mc->name[0] ? mc->name : m->id;
    char msg[96];
    if (is_new) {
        snprintf(msg, sizeof(msg), "%s heard (%d dBm) %s", label, m->rssi, m->summary);
        ntfy_notify("Starred meter in range", msg);
    } else if (strcmp(prev_status, m->status_txt) != 0 && m->status_txt[0]) {
        snprintf(msg, sizeof(msg), "%s status: %s", label, m->status_txt);
        ntfy_notify(m->alarm ? "Meter alarm" : "Meter status", msg);
    }
}

static void process_frame(const Frame& f, int16_t rssi) {
    const MeterConf* mc = decode_configured(f, false);
    const DecodeResult& r = s_dec.res;
    switch (r.status) {
    case DecodeStatus::Ok: g_app.decoded++; break;
    case DecodeStatus::Encrypted: g_app.encrypted++; break;
    default: g_app.undecoded++; break;
    }
    count_minute();

    double pos[2];
    bool have_pos = gnss_position(&pos[0], &pos[1]);
    Meter* prev = meters_find(r.id);
    char prev_status[sizeof(prev->status_txt)];
    snprintf(prev_status, sizeof(prev_status), "%s", prev ? prev->status_txt : "");
    bool is_new = false;
    Meter* m = meters_update(f, r, rssi, have_pos ? pos : nullptr, &is_new);

    FeedEntry fe;
    memset(&fe, 0, sizeof(fe));
    fe.ms = millis();
    snprintf(fe.id, sizeof(fe.id), "%s", r.id);
    memcpy(fe.mfct, r.mfct, sizeof(fe.mfct));
    fe.rssi = rssi;
    fe.mode = f.mode;
    fe.status = r.status;
    fe.driver = r.driver ? r.driver->name : nullptr;
    if (m) memcpy(fe.summary, m->summary, sizeof(fe.summary));
    feed_add(fe);

    const char* name = mc && mc->name[0] ? mc->name : nullptr;
    console_telegram(f, s_dec, rssi, name);
    sdlog_telegram(f, s_dec, rssi, have_pos ? pos : nullptr);
    mqtt_publish_telegram(f, s_dec, rssi, name);
    if (m) notify_if_needed(m, mc, is_new, prev_status);

    s_event.seq++;
    s_event.ms = millis();
    snprintf(s_event.id, sizeof(s_event.id), "%s", r.id);
    s_event.is_new = is_new;
    s_event.starred = mc && mc->starred;
    s_event.alarm = m && m->alarm;
    s_event.decoded = r.status == DecodeStatus::Ok;
    board_telegram_alert(s_event);
}

static void handle_capture(const RadioCapture& c) {
    g_app.captures++;
    Frame f;
    size_t used = 0;
    CaptureStatus st = frame_from_capture(c.data, c.len, c.band, &f, &used);
    if (st != CaptureStatus::Ok) {
        if (st == CaptureStatus::CrcError) g_app.crc_errors++;
        else if (st == CaptureStatus::Truncated) g_app.truncated++;
        else g_app.bad_coding++;
        console_capture_error(c, st);
        return;
    }
    f.rssi = c.rssi_dbm;
    g_app.frames++;
    if (f.mode == LinkMode::C1) g_app.c1++;
    else if (f.mode == LinkMode::T1) g_app.t1++;
    else if (f.mode == LinkMode::S1) g_app.s1++;
    process_frame(f, c.rssi_dbm);
}

void app_loop() {
    for (int budget = 0; budget < 4 && radio_receive(&s_cap, 0); ++budget) handle_capture(s_cap);
    update_per_min();
    if (g_cfg.band_mode == (uint8_t)BandMode::Hop && radio_ready()) {
        uint32_t dwell = (uint32_t)(g_cfg.hop_s ? g_cfg.hop_s : 30) * 1000u;
        if (millis() - s_hop_ms >= dwell && !radio_busy()) {
            s_hop_ms = millis();
            radio_set_band(radio_band() == RadioBand::CT ? RadioBand::S : RadioBand::CT);
        }
    }
}

const Decoder* app_redecode(const Meter* m) {
    if (!m || !m->frame_len) return nullptr;
    Frame f;
    memset(&f, 0, sizeof(f));
    memcpy(f.data, m->frame, m->frame_len);
    f.len = m->frame_len;
    f.mode = (LinkMode)m->frame_mode;
    f.format = (FrameFormat)m->frame_format;
    f.crc_checked = true;
    f.rssi = m->rssi;
    decode_configured(f, false);
    return &s_dec;
}

const Decoder* app_analyze(const char* hex, const char* key_hex, const char* driver, bool* frame_ok) {
    static uint8_t bytes[2 * WMB_FRAME_MAX];
    *frame_ok = false;
    size_t n = hex_to_bytes(hex, bytes, sizeof(bytes));
    Frame f;
    if (!n || !frame_from_bytes(bytes, n, &f)) return nullptr;
    *frame_ok = true;
    uint8_t key[16];
    uint8_t key_len = 0;
    if (key_hex && key_hex[0] && !parse_key_hex(key_hex, key, &key_len)) key_len = 0;
    if (!key_len && (!driver || !driver[0])) {
        decode_configured(f, true);
        return &s_dec;
    }
    DecodeOptions o;
    memset(&o, 0, sizeof(o));
    o.generic_fallback = true;
    o.simulated = true;
    o.now_unix = app_unix_time();
    if (key_len) {
        o.key = key;
        o.key_len = key_len;
    }
    if (driver && driver[0] && strcmp(driver, "auto") != 0) o.forced_driver = driver_by_name(driver);
    wmbus_decode(&s_dec, f, o);
    return &s_dec;
}

void app_meter_config_changed(const char* id) {
    Meter* m = meters_find(id);
    if (!m) return;
    // Decode the last frame with the new key/driver so lists update at once.
    const Decoder* d = app_redecode(m);
    if (!d) return;
    const DecodeResult& r = d->res;
    m->driver = r.driver;
    m->status = r.status;
    m->decrypt = r.decrypt;
    if (r.status == DecodeStatus::Ok) {
        result_summary(r, m->summary, sizeof(m->summary));
        result_status(r, m->status_txt, sizeof(m->status_txt));
        m->alarm = m->status_txt[0] && strcmp(m->status_txt, "OK") != 0;
    } else {
        m->summary[0] = 0;
        m->status_txt[0] = 0;
        m->alarm = false;
    }
}

} // namespace wmb
