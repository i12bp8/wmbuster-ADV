// Host stand-ins for the firmware modules the UI talks to (radio, network,
// settings, board). Meters and decoding use the real code.
#include <math.h>
#include <string.h>

#include "radio/radio.h"
#include "wmbus/engine.h"
#include "app/app.h"
#include "app/meters.h"
#include "app/sdlog.h"
#include "app/settings.h"
#include "hw/board.h"
#include "hw/gnss.h"
#include "net/mqtt.h"
#include "net/net.h"
#include "ui/ui_view.h"

uint32_t g_now_ms = 100000;
uint32_t millis() { return g_now_ms; }

namespace wmb {

Settings g_cfg;
AppStats g_app;
static AppEvent s_event;
static MeterConf s_conf[64];
static int s_conf_n = 0;
static Decoder s_dec;

void settings_defaults(Settings* s) {
    memset(s, 0, sizeof(*s));
    s->band_mode = 0;
    s->hop_s = 30;
    s->sx_bw_idx = 2;
    s->boosted_gain = true;
    s->wifi_mode = 1;
    strcpy(s->ap_pass, "wmbuster");
    strcpy(s->hostname, "wmbuster");
    s->mqtt_enabled = true;
    s->mqtt_port = 1883;
    s->sd_log = true;
    s->serial_out = 1;
    s->brightness = 140;
    s->dim_s = 90;
    s->sound = true;
}
void settings_save() {}
float settings_sx_bw(uint8_t idx) {
    static const float B[] = { 156.2f, 187.2f, 234.3f, 312.0f, 373.6f, 467.0f };
    return B[idx % 6];
}
uint8_t settings_sx_bw_count() { return 6; }

const MeterConf* meterconf_find(const char* id) {
    for (int i = 0; i < s_conf_n; ++i)
        if (!strcmp(s_conf[i].id, id)) return &s_conf[i];
    return nullptr;
}
bool meterconf_put(const MeterConf& c) {
    for (int i = 0; i < s_conf_n; ++i)
        if (!strcmp(s_conf[i].id, c.id)) {
            s_conf[i] = c;
            return true;
        }
    if (s_conf_n >= 64) return false;
    s_conf[s_conf_n++] = c;
    return true;
}
bool parse_key_hex(const char* hex, uint8_t* out, uint8_t* len) {
    size_t n = hex_to_bytes(hex, out, 16);
    if (n != 16 && n != 8) return false;
    *len = (uint8_t)n;
    return true;
}

const AppEvent& app_last_event() { return s_event; }
void preview_event(const char* id, bool is_new, bool alarm) {
    s_event.seq++;
    s_event.ms = g_now_ms;
    snprintf(s_event.id, sizeof(s_event.id), "%s", id);
    s_event.is_new = is_new;
    s_event.alarm = alarm;
}
const char* app_band_label() { return "C1/T1"; }
void app_apply_radio_band() {}
void app_meter_config_changed(const char*) {}
const Decoder* app_redecode(const Meter* m) {
    if (!m || !m->frame_len) return nullptr;
    Frame f;
    memset(&f, 0, sizeof(f));
    memcpy(f.data, m->frame, m->frame_len);
    f.len = m->frame_len;
    f.mode = (LinkMode)m->frame_mode;
    f.format = (FrameFormat)m->frame_format;
    DecodeOptions o;
    memset(&o, 0, sizeof(o));
    o.generic_fallback = true;
    const MeterConf* c = meterconf_find(m->id);
    if (c && c->key_len) {
        o.key = c->key;
        o.key_len = c->key_len;
    }
    wmbus_decode(&s_dec, f, o);
    return &s_dec;
}

bool radio_ready() { return true; }
const char* radio_chip_name() { return "SX1262"; }
RadioBand radio_band() { return RadioBand::CT; }
const char* radio_band_name(RadioBand b) { return b == RadioBand::CT ? "C1/T1" : "S1"; }
void radio_get_stats(RadioStats* s) {
    memset(s, 0, sizeof(*s));
    s->syncs = 5120;
    s->captures = 1432;
    s->noise = 3688;
    s->noise_floor = -108;
}

void net_status(NetStatus* o) {
    memset(o, 0, sizeof(*o));
    o->ap_on = true;
    strcpy(o->ap_ssid, "wM-Buster-1A2B");
    strcpy(o->ap_ip, "192.168.4.1");
    o->ap_clients = 1;
    o->sta_connected = true;
    strcpy(o->sta_ip, "192.168.1.57");
    strcpy(o->sta_ssid, "HomeNet");
    o->mqtt_connected = true;
}
void net_apply_settings() {}
void mqtt_rediscover(const char*) {}

void gnss_info(GnssInfo* o) {
    memset(o, 0, sizeof(*o));
    o->present = true;
    o->fix = true;
    o->sats = 9;
    o->lat = 52.3702;
    o->lon = 4.8952;
}
bool gnss_position(double* lat, double* lon) {
    *lat = 52.3702;
    *lon = 4.8952;
    return true;
}

int board_battery_pct() { return 78; }
bool board_charging() { return false; }
void board_set_brightness(uint8_t) {}
bool sdlog_card_ok() { return true; }

void ui_hook_tone(uint16_t, uint16_t) {}

} // namespace wmb
