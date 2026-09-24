// wM-Buster ADV — web UI and JSON API.
// GPL-3.0
#include "net.h"

#include <Arduino.h>
#include <SD.h>
#include <Update.h>
#include <WebServer.h>
#include <math.h>
#include <string.h>

#include "mqtt.h"
#include "web_page.h"
#include "radio/radio.h"
#include "../app/app.h"
#include "../app/console.h"
#include "../app/meters.h"
#include "../app/sdlog.h"
#include "../app/settings.h"
#include "../app/scratch.h"
#include "../hw/board.h"
#include "../hw/gnss.h"
#include "../ui/ui.h"
#include "../version.h"

namespace wmb {

static WebServer s_srv(80);
static uint32_t s_apply_net_at = 0;
static uint32_t s_reboot_at = 0;
static const char* PASSWORD_MASK = "********";

// ---------------------------------------------------------------------------
// Streaming JSON writer (chunked transfer, small fixed buffer)
// ---------------------------------------------------------------------------
class Json {
public:
    void begin(int code = 200) {
        s_srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
        s_srv.sendHeader("Cache-Control", "no-store");
        s_srv.send(code, "application/json", "");
        n_ = 0;
        depth_ = 0;
        first_[0] = true;
    }
    void end() {
        flush();
        s_srv.sendContent("");
    }
    void obj() { open('{'); }
    void obj(const char* k) { key(k); open_nocomma('{'); }
    void arr() { open('['); }
    void arr(const char* k) { key(k); open_nocomma('['); }
    void close_obj() { close('}'); }
    void close_arr() { close(']'); }
    void kv(const char* k, const char* v) { key(k); str(v); }
    void kv(const char* k, bool v) { key(k); raw(v ? "true" : "false"); }
    void kv(const char* k, int32_t v) { key(k); num(v); }
    void kv(const char* k, uint32_t v) { key(k); num((double)v); }
    void kv(const char* k, double v, int prec) {
        key(k);
        if (isnan(v)) { raw("null"); return; }
        char b[32];
        snprintf(b, sizeof(b), "%.*f", prec, v);
        raw(b);
    }
    void kvraw(const char* k, const char* v) { key(k); raw(v); }
    void item(const char* v) { comma(); str(v); }
    void raw(const char* s) { while (*s) put(*s++); }
    void str(const char* s) {
        put('"');
        for (const char* p = s ? s : ""; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { put('\\'); put((char)c); }
            else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); raw(b); }
            else put((char)c);
        }
        put('"');
    }

private:
    char buf_[1400];
    size_t n_ = 0;
    int depth_ = 0;
    bool first_[8];
    void put(char c) {
        if (n_ >= sizeof(buf_)) flush();
        buf_[n_++] = c;
    }
    void flush() {
        if (!n_) return;
        s_srv.sendContent(buf_, n_);
        n_ = 0;
    }
    void comma() {
        if (!first_[depth_]) put(',');
        first_[depth_] = false;
    }
    void key(const char* k) { comma(); str(k); put(':'); }
    void open(char c) { comma(); open_nocomma(c); }
    void open_nocomma(char c) {
        put(c);
        if (depth_ < 7) depth_++;
        first_[depth_] = true;
    }
    void close(char c) {
        put(c);
        if (depth_ > 0) depth_--;
    }
    void num(int32_t v) { char b[16]; snprintf(b, sizeof(b), "%ld", (long)v); raw(b); }
    void num(double v) { char b[24]; snprintf(b, sizeof(b), "%.0f", v); raw(b); }
};

static Json s_json;

static bool auth() {
    if (!g_cfg.web_pass[0]) return true;
    if (s_srv.authenticate("admin", g_cfg.web_pass)) return true;
    s_srv.requestAuthentication(BASIC_AUTH, "wM-Buster");
    return false;
}

static void send_error(int code, const char* msg) { s_srv.send(code, "text/plain", msg); }

static const char* status_word(DecodeStatus s) {
    switch (s) {
    case DecodeStatus::Ok: return "ok";
    case DecodeStatus::Encrypted: return "encrypted";
    case DecodeStatus::CompactUnknown: return "compact";
    case DecodeStatus::MfctPayload: return "manufacturer";
    case DecodeStatus::NoPayload: return "no payload";
    default: return "bad header";
    }
}

static void write_fields(const DecodeResult& r) {
    s_json.arr("fields");
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& f = r.fields[i];
        if (f.hidden) continue;
        char v[160];
        field_format(&f, v, sizeof(v));
        s_json.obj();
        s_json.kv("name", f.name);
        s_json.kv("value", v);
        s_json.close_obj();
    }
    s_json.close_arr();
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static void h_root() {
    if (!auth()) return;
    s_srv.sendHeader("Content-Encoding", "gzip");
    s_srv.sendHeader("Cache-Control", "no-cache");
    s_srv.send_P(200, "text/html", (const char*)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

static void h_status() {
    if (!auth()) return;
    RadioStats rs;
    radio_get_stats(&rs);
    NetStatus ns;
    net_status(&ns);
    GnssInfo gi;
    gnss_info(&gi);
    s_json.begin();
    s_json.obj();
    s_json.kv("version", WMB_VERSION);
    s_json.kv("uptime", (uint32_t)(millis() / 1000));
    s_json.kv("heap", (uint32_t)ESP.getFreeHeap());
    s_json.kv("heap_min", (uint32_t)ESP.getMinFreeHeap());
    s_json.kv("meters", meters_count());
    s_json.kv("battery", board_battery_pct());
    s_json.kv("time", app_unix_time());
    s_json.obj("radio");
    s_json.kv("chip", radio_chip_name());
    s_json.kv("band", radio_band_name(radio_band()));
    s_json.kv("band_mode", app_band_label());
    s_json.kv("syncs", rs.syncs);
    s_json.kv("captures", rs.captures);
    s_json.kv("noise", rs.noise);
    s_json.kv("dropped", rs.dropped);
    s_json.kv("overflows", rs.overflows);
    s_json.kv("timeouts", rs.timeouts);
    s_json.kv("noise_floor", (int)rs.noise_floor);
    s_json.close_obj();
    s_json.obj("app");
    s_json.kv("frames", g_app.frames);
    s_json.kv("decoded", g_app.decoded);
    s_json.kv("encrypted", g_app.encrypted);
    s_json.kv("undecoded", g_app.undecoded);
    s_json.kv("crc_errors", g_app.crc_errors);
    s_json.kv("bad_coding", g_app.bad_coding);
    s_json.kv("truncated", g_app.truncated);
    s_json.kv("c1", g_app.c1);
    s_json.kv("t1", g_app.t1);
    s_json.kv("s1", g_app.s1);
    s_json.kv("per_min", g_app.per_min);
    s_json.close_obj();
    s_json.obj("net");
    s_json.kv("ap_on", ns.ap_on);
    s_json.kv("ap_ssid", ns.ap_ssid);
    s_json.kv("ap_ip", ns.ap_ip);
    s_json.kv("ap_clients", ns.ap_clients);
    s_json.kv("sta_ssid", ns.sta_ssid);
    s_json.kv("sta_ip", ns.sta_ip);
    s_json.kv("sta_rssi", ns.sta_rssi);
    s_json.kv("time_synced", ns.time_synced);
    s_json.close_obj();
    s_json.obj("mqtt");
    s_json.kv("enabled", g_cfg.mqtt_enabled);
    s_json.kv("connected", mqtt_connected());
    s_json.kv("published", mqtt_published());
    s_json.kv("error", mqtt_last_error());
    s_json.close_obj();
    s_json.obj("gnss");
    s_json.kv("present", gi.present);
    s_json.kv("fix", gi.fix);
    s_json.kv("sats", (int)gi.sats);
    s_json.kv("lat", gi.lat, 6);
    s_json.kv("lon", gi.lon, 6);
    s_json.close_obj();
    s_json.obj("sd");
    s_json.kv("ok", sdlog_card_ok());
    s_json.kv("free_mb", (double)sdlog_card_free() / 1048576.0, 0);
    s_json.kv("lines", sdlog_lines());
    s_json.close_obj();
    s_json.close_obj();
    s_json.end();
}

static void write_meter_summary(const Meter* m) {
    const MeterConf* c = meterconf_find(m->id);
    const char* mn = manufacturer_name(m->mfct_code);
    s_json.kv("id", m->id);
    s_json.kv("name", c ? c->name : "");
    s_json.kv("mfct", m->mfct);
    s_json.kv("mfct_name", mn ? mn : "");
    s_json.kv("driver", m->driver ? m->driver->name : "");
    s_json.kv("media", media_name(m->type, m->mfct_code));
    s_json.kv("value", m->summary);
    s_json.kv("status", status_word(m->status));
    s_json.kv("decrypt", decrypt_status_name(m->decrypt));
    s_json.kv("status_txt", m->status_txt);
    s_json.kv("alarm", m->alarm);
    s_json.kv("rssi", (int)m->rssi);
    s_json.kv("rssi_best", (int)m->rssi_best);
    s_json.kv("count", m->count);
    s_json.kv("age", (uint32_t)((millis() - m->last_ms) / 1000));
    s_json.kv("interval", m->interval_ms / 1000);
    s_json.kv("mode", link_mode_name(m->mode));
    s_json.kv("starred", c && c->starred);
    s_json.kv("key", c && c->key_len);
    if (m->fix) {
        s_json.kv("lat", m->lat, 6);
        s_json.kv("lon", m->lon, 6);
    }
}

static void h_meters() {
    if (!auth()) return;
    s_json.begin();
    s_json.arr();
    for (int i = 0; i < meters_count(); ++i) {
        s_json.obj();
        write_meter_summary(meters_at(i));
        s_json.close_obj();
    }
    s_json.close_arr();
    s_json.end();
}

static void h_meter() {
    if (!auth()) return;
    const Meter* m = meters_find(s_srv.arg("id").c_str());
    if (!m) return send_error(404, "unknown meter");
    const Decoder* d = app_redecode(m);
    s_json.begin();
    s_json.obj();
    write_meter_summary(m);
    char b[8];
    snprintf(b, sizeof(b), "%02X", m->version);
    s_json.kv("version", b);
    snprintf(b, sizeof(b), "%02X", m->type);
    s_json.kv("type", b);
    static char hex[2 * WMB_FRAME_MAX + 2];
    bytes_to_hex(m->frame, m->frame_len, hex, sizeof(hex));
    s_json.kv("hex", hex);
    if (d) {
        write_fields(d->res);
        if (d->res.status == DecodeStatus::Ok) {
            char* js = scratch();
            const MeterConf* c = meterconf_find(m->id);
            format_json(*d, m->rssi, c && c->name[0] ? c->name : nullptr, m->fix ? &m->lat : nullptr, js, SCRATCH_LEN);
            s_json.kv("json", js);
        }
    }
    const MeterConf* c = meterconf_find(m->id);
    s_json.obj("config");
    if (c) {
        s_json.kv("id", c->id);
        s_json.kv("name", c->name);
        s_json.kv("driver", c->driver);
        s_json.kv("key", c->key_len != 0);
        s_json.kv("starred", c->starred);
    }
    s_json.close_obj();
    s_json.close_obj();
    s_json.end();
}

static void h_meter_post() {
    if (!auth()) return;
    String id = s_srv.arg("id");
    id.toLowerCase();
    if (!meter_id_valid(id.c_str())) return send_error(400, "invalid id");
    const MeterConf* old = meterconf_find(id.c_str());
    MeterConf c;
    if (old) c = *old;
    else {
        memset(&c, 0, sizeof(c));
        snprintf(c.id, sizeof(c.id), "%s", id.c_str());
    }
    if (s_srv.hasArg("name")) snprintf(c.name, sizeof(c.name), "%s", s_srv.arg("name").c_str());
    if (s_srv.hasArg("driver")) {
        String dr = s_srv.arg("driver");
        if (dr.length() && dr != "auto" && !driver_by_name(dr.c_str())) return send_error(400, "unknown driver");
        snprintf(c.driver, sizeof(c.driver), "%s", dr == "auto" ? "" : dr.c_str());
    }
    if (s_srv.hasArg("key")) {
        String k = s_srv.arg("key");
        k.trim();
        if (k.equalsIgnoreCase("none") || k.equalsIgnoreCase("nokey")) c.key_len = 0;
        else if (k.length() && !parse_key_hex(k.c_str(), c.key, &c.key_len))
            return send_error(400, "key must be 32 hex digits (AES) or 16 (DES)");
    }
    if (s_srv.hasArg("starred")) c.starred = s_srv.arg("starred") == "1";
    if (!meterconf_put(c)) return send_error(507, "meter table full");
    app_meter_config_changed(c.id);
    mqtt_rediscover(c.id);
    s_srv.send(200, "application/json", "{\"ok\":true}");
}

static void h_meter_delete() {
    if (!auth()) return;
    String id = s_srv.arg("id");
    meterconf_del(id.c_str());
    app_meter_config_changed(id.c_str());
    s_srv.send(200, "application/json", "{\"ok\":true}");
}

static void h_feed() {
    if (!auth()) return;
    s_json.begin();
    s_json.arr();
    int n = feed_count() < 40 ? feed_count() : 40;
    for (int i = 0; i < n; ++i) {
        const FeedEntry* e = feed_at(i);
        const Meter* m = meters_find(e->id);
        s_json.obj();
        s_json.kv("age", (uint32_t)((millis() - e->ms) / 1000));
        s_json.kv("id", e->id);
        s_json.kv("mfct", e->mfct);
        s_json.kv("driver", e->driver ? e->driver : "");
        s_json.kv("mode", link_mode_name(e->mode));
        s_json.kv("rssi", (int)e->rssi);
        s_json.kv("value", e->summary);
        s_json.kv("status", status_word(e->status));
        s_json.kv("status_txt", m ? m->status_txt : "");
        s_json.kv("alarm", m && m->alarm);
        s_json.kv("decrypt", m ? decrypt_status_name(m->decrypt) : "");
        s_json.close_obj();
    }
    s_json.close_arr();
    s_json.end();
}

static void h_analyze() {
    if (!auth()) return;
    bool frame_ok = false;
    const Decoder* d = app_analyze(s_srv.arg("hex").c_str(), s_srv.arg("key").c_str(), s_srv.arg("driver").c_str(),
                                   &frame_ok);
    s_json.begin();
    s_json.obj();
    s_json.kv("frame", frame_ok && d);
    if (d) {
        const DecodeResult& r = d->res;
        const char* mn = manufacturer_name(r.mfct_code);
        char b[8];
        s_json.kv("id", r.id);
        s_json.kv("mfct", r.mfct);
        s_json.kv("mfct_name", mn ? mn : "");
        snprintf(b, sizeof(b), "%02X", r.version);
        s_json.kv("version", b);
        snprintf(b, sizeof(b), "%02X", r.type);
        s_json.kv("type", b);
        snprintf(b, sizeof(b), "%02X", r.ci);
        s_json.kv("ci", b);
        s_json.kv("media", r.media ? r.media : "");
        s_json.kv("mode", link_mode_name(r.mode));
        s_json.kv("driver", r.driver ? r.driver->name : "");
        s_json.kv("status", status_word(r.status));
        s_json.kv("decrypt", decrypt_status_name(r.decrypt));
        char stt[64];
        result_status(r, stt, sizeof(stt));
        s_json.kv("status_txt", stt);
        s_json.kv("alarm", stt[0] && strcmp(stt, "OK") != 0);
        write_fields(r);
        if (r.status == DecodeStatus::Ok) {
            char* js = scratch();
            format_json(*d, 0, nullptr, nullptr, js, SCRATCH_LEN);
            s_json.kv("json", js);
        }
    }
    s_json.close_obj();
    s_json.end();
}

static void h_drivers() {
    if (!auth()) return;
    s_json.begin();
    s_json.arr();
    for (size_t i = 0; i < DRIVERS_LEN; ++i) s_json.item(DRIVERS[i].name);
    s_json.close_arr();
    s_json.end();
}

static void pw(const char* k, const char* v) { s_json.kv(k, v[0] ? PASSWORD_MASK : ""); }

static void h_settings_get() {
    if (!auth()) return;
    const Settings& s = g_cfg;
    s_json.begin();
    s_json.obj();
    s_json.kv("radio_hw", (int)s.radio_hw);
    s_json.kv("band_mode", (int)s.band_mode);
    s_json.kv("hop_s", (int)s.hop_s);
    s_json.kv("sx_bw_idx", (int)s.sx_bw_idx);
    s_json.kv("boosted_gain", s.boosted_gain);
    s_json.kv("wifi_mode", (int)s.wifi_mode);
    s_json.kv("wifi_ssid", s.wifi_ssid);
    pw("wifi_pass", s.wifi_pass);
    s_json.kv("ap_ssid", s.ap_ssid);
    pw("ap_pass", s.ap_pass);
    s_json.kv("hostname", s.hostname);
    pw("web_pass", s.web_pass);
    s_json.kv("mqtt_enabled", s.mqtt_enabled);
    s_json.kv("mqtt_host", s.mqtt_host);
    s_json.kv("mqtt_port", (int)s.mqtt_port);
    s_json.kv("mqtt_user", s.mqtt_user);
    pw("mqtt_pass", s.mqtt_pass);
    s_json.kv("mqtt_prefix", s.mqtt_prefix);
    s_json.kv("mqtt_ha", s.mqtt_ha);
    s_json.kv("mqtt_raw", s.mqtt_raw);
    s_json.kv("ntfy_url", s.ntfy_url);
    s_json.kv("sd_log", s.sd_log);
    s_json.kv("serial_out", (int)s.serial_out);
    s_json.kv("theme", (int)s.theme);
    s_json.kv("brightness", (int)s.brightness);
    s_json.kv("dim_s", (int)s.dim_s);
    s_json.kv("sound", s.sound);
    s_json.kv("beep_all", s.beep_all);
    s_json.kv("tz_min", (int)s.tz_min);
    s_json.close_obj();
    s_json.end();
}

static void arg_str(const char* name, char* dst, size_t cap, bool password = false) {
    if (!s_srv.hasArg(name)) return;
    String v = s_srv.arg(name);
    if (password && v == PASSWORD_MASK) return;
    snprintf(dst, cap, "%s", v.c_str());
}

static bool arg_bool(const char* name, bool cur) {
    if (!s_srv.hasArg(name)) return cur;
    String v = s_srv.arg(name);
    return v == "1" || v == "true" || v == "on";
}

static long arg_int(const char* name, long cur, long lo, long hi) {
    if (!s_srv.hasArg(name)) return cur;
    long v = s_srv.arg(name).toInt();
    return v < lo ? lo : v > hi ? hi : v;
}

static void h_settings_post() {
    if (!auth()) return;
    Settings old = g_cfg;
    Settings& s = g_cfg;
    s.radio_hw = (uint8_t)arg_int("radio_hw", s.radio_hw, 0, 2);
    s.band_mode = (uint8_t)arg_int("band_mode", s.band_mode, 0, 2);
    s.hop_s = (uint16_t)arg_int("hop_s", s.hop_s, 5, 3600);
    s.sx_bw_idx = (uint8_t)arg_int("sx_bw_idx", s.sx_bw_idx, 0, settings_sx_bw_count() - 1);
    s.boosted_gain = arg_bool("boosted_gain", s.boosted_gain);
    s.wifi_mode = (uint8_t)arg_int("wifi_mode", s.wifi_mode, 0, 3);
    arg_str("wifi_ssid", s.wifi_ssid, sizeof(s.wifi_ssid));
    arg_str("wifi_pass", s.wifi_pass, sizeof(s.wifi_pass), true);
    arg_str("ap_ssid", s.ap_ssid, sizeof(s.ap_ssid));
    arg_str("ap_pass", s.ap_pass, sizeof(s.ap_pass), true);
    arg_str("hostname", s.hostname, sizeof(s.hostname));
    arg_str("web_pass", s.web_pass, sizeof(s.web_pass), true);
    s.mqtt_enabled = arg_bool("mqtt_enabled", s.mqtt_enabled);
    arg_str("mqtt_host", s.mqtt_host, sizeof(s.mqtt_host));
    s.mqtt_port = (uint16_t)arg_int("mqtt_port", s.mqtt_port, 1, 65535);
    arg_str("mqtt_user", s.mqtt_user, sizeof(s.mqtt_user));
    arg_str("mqtt_pass", s.mqtt_pass, sizeof(s.mqtt_pass), true);
    arg_str("mqtt_prefix", s.mqtt_prefix, sizeof(s.mqtt_prefix));
    s.mqtt_ha = arg_bool("mqtt_ha", s.mqtt_ha);
    s.mqtt_raw = arg_bool("mqtt_raw", s.mqtt_raw);
    arg_str("ntfy_url", s.ntfy_url, sizeof(s.ntfy_url));
    s.sd_log = arg_bool("sd_log", s.sd_log);
    s.serial_out = (uint8_t)arg_int("serial_out", s.serial_out, 0, 3);
    s.theme = (uint8_t)arg_int("theme", s.theme, 0, 3);
    s.brightness = (uint8_t)arg_int("brightness", s.brightness, 10, 255);
    s.dim_s = (uint16_t)arg_int("dim_s", s.dim_s, 0, 3600);
    s.sound = arg_bool("sound", s.sound);
    s.beep_all = arg_bool("beep_all", s.beep_all);
    s.tz_min = (int16_t)arg_int("tz_min", s.tz_min, -720, 840);
    if (strlen(s.ap_pass) > 0 && strlen(s.ap_pass) < 8) return send_error(400, "AP password needs 8+ characters");
    settings_save();

    bool reboot = s.radio_hw != old.radio_hw || s.sx_bw_idx != old.sx_bw_idx || s.boosted_gain != old.boosted_gain;
    if (s.band_mode != old.band_mode) app_apply_radio_band();
    if (s.brightness != old.brightness) board_set_brightness(s.brightness);
    if (s.theme != old.theme) ui_apply_theme();
    bool net = s.wifi_mode != old.wifi_mode || strcmp(s.wifi_ssid, old.wifi_ssid) || strcmp(s.wifi_pass, old.wifi_pass) ||
               strcmp(s.ap_ssid, old.ap_ssid) || strcmp(s.ap_pass, old.ap_pass) || strcmp(s.hostname, old.hostname);
    if (net) s_apply_net_at = millis() + 800;  // answer first, then switch WiFi
    else mqtt_apply_settings();
    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":true,\"reboot\":%s}", reboot ? "true" : "false");
    s_srv.send(200, "application/json", out);
}

static void h_keys_post() {
    if (!auth()) return;
    int n = meterconf_import_text(s_srv.arg("text").c_str());
    for (int i = 0; i < meters_count(); ++i) app_meter_config_changed(meters_at(i)->id);
    char out[48];
    snprintf(out, sizeof(out), "{\"imported\":%d}", n);
    s_srv.send(200, "application/json", out);
}

static void h_keys_get() {
    if (!auth()) return;
    s_srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_srv.sendHeader("Content-Disposition", "attachment; filename=keys.txt");
    s_srv.send(200, "text/plain", "");
    s_srv.sendContent("# id,key,name,driver\n");
    for (int i = 0; i < meterconf_count(); ++i) {
        const MeterConf* c = meterconf_at(i);
        char key[40] = "NOKEY", line[128];
        if (c->key_len) key_to_hex(c->key, c->key_len, key, sizeof(key));
        snprintf(line, sizeof(line), "%s,%s,%s,%s\n", c->id, key, c->name, c->driver[0] ? c->driver : "auto");
        s_srv.sendContent(line);
    }
    s_srv.sendContent("");
}

static void h_log() {
    if (!auth()) return;
    String f = s_srv.arg("f");
    const char* path = f == "json" ? "/wmbuster/telegrams.jsonl" : f == "ward" ? "/wmbuster/wardrive.csv"
                                                                               : "/wmbuster/telegrams.rtl";
    if (!sdlog_card_ok()) return send_error(404, "no SD card");
    File file = SD.open(path, FILE_READ);
    if (!file) return send_error(404, "no log yet");
    const char* name = strrchr(path, '/') + 1;
    char cd[80];
    snprintf(cd, sizeof(cd), "attachment; filename=%s", name);
    s_srv.sendHeader("Content-Disposition", cd);
    s_srv.streamFile(file, "application/octet-stream");
    file.close();
}

static void h_reboot() {
    if (!auth()) return;
    s_srv.send(200, "application/json", "{\"ok\":true}");
    s_reboot_at = millis() + 500;
}

static void h_clear() {
    if (!auth()) return;
    meters_clear();
    feed_clear();
    s_srv.send(200, "application/json", "{\"ok\":true}");
}

static void h_update_done() {
    if (!auth()) return;
    bool ok = !Update.hasError();
    s_srv.send(ok ? 200 : 500, "text/plain", ok ? "OK" : Update.errorString());
    if (ok) s_reboot_at = millis() + 800;
}

static void h_update_upload() {
    if (g_cfg.web_pass[0] && !s_srv.authenticate("admin", g_cfg.web_pass)) return;
    HTTPUpload& up = s_srv.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[OTA] %u bytes, rebooting\n", up.totalSize);
        else Update.printError(Serial);
    }
}

void web_begin() {
    s_srv.on("/", HTTP_GET, h_root);
    s_srv.on("/api/status", HTTP_GET, h_status);
    s_srv.on("/api/meters", HTTP_GET, h_meters);
    s_srv.on("/api/meter", HTTP_GET, h_meter);
    s_srv.on("/api/meter", HTTP_POST, h_meter_post);
    s_srv.on("/api/meter/delete", HTTP_POST, h_meter_delete);
    s_srv.on("/api/feed", HTTP_GET, h_feed);
    s_srv.on("/api/analyze", HTTP_POST, h_analyze);
    s_srv.on("/api/drivers", HTTP_GET, h_drivers);
    s_srv.on("/api/settings", HTTP_GET, h_settings_get);
    s_srv.on("/api/settings", HTTP_POST, h_settings_post);
    s_srv.on("/api/keys", HTTP_GET, h_keys_get);
    s_srv.on("/api/keys", HTTP_POST, h_keys_post);
    s_srv.on("/api/log", HTTP_GET, h_log);
    s_srv.on("/api/reboot", HTTP_POST, h_reboot);
    s_srv.on("/api/clear", HTTP_POST, h_clear);
    s_srv.on("/update", HTTP_POST, h_update_done, h_update_upload);
    s_srv.onNotFound([]() {
        // Captive-portal friendly: everything else gets the app.
        if (s_srv.uri().startsWith("/api/")) send_error(404, "not found");
        else h_root();
    });
    s_srv.begin();
}

void web_loop() {
    s_srv.handleClient();
    uint32_t now = millis();
    if (s_apply_net_at && (int32_t)(now - s_apply_net_at) >= 0) {
        s_apply_net_at = 0;
        net_apply_settings();
    }
    if (s_reboot_at && (int32_t)(now - s_reboot_at) >= 0) {
        delay(50);
        ESP.restart();
    }
}

} // namespace wmb
