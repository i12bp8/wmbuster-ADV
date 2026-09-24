// wM-Buster ADV — USB serial console.
// GPL-3.0
#include "console.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "meters.h"
#include "settings.h"
#include "sdlog.h"
#include "scratch.h"
#include "../net/net.h"

namespace wmb {

void format_timestamp(char* out, size_t cap) {
    time_t t = time(nullptr);
    if (t < 1600000000) {
        if (cap) out[0] = 0;
        return;
    }
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

size_t format_rtlwmbus(const Frame& f, int16_t rssi, char* out, size_t cap) {
    if (f.mode == LinkMode::MBus || f.len < 10) return 0;
    const char* mode = f.mode == LinkMode::C1 ? "C1" : f.mode == LinkMode::S1 ? "S1" : "T1";
    char ts[40];
    time_t t = time(nullptr);
    if (t < 1600000000) t = 0;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    int o = snprintf(out, cap, "%s;1;1;%s.000;%d;%d;%02X%02X%02X%02X;0x", mode, ts, rssi, rssi, f.data[7], f.data[6],
                     f.data[5], f.data[4]);
    if (o < 0 || (size_t)o >= cap) return 0;
    bytes_to_hex(f.data, f.len, out + o, cap - (size_t)o, false);
    return strlen(out);
}

size_t format_json(const Decoder& d, int16_t rssi, const char* name, const double* pos, char* out, size_t cap) {
    char ts[32];
    format_timestamp(ts, sizeof(ts));
    size_t n = result_to_json(&d.res, out, cap, name, ts[0] ? ts : nullptr);
    if (n < 2 || out[n - 1] != '}' || n + 80 >= cap) return n;
    n--;
    n += (size_t)snprintf(out + n, cap - n, ",\"rssi_dbm\":%d", rssi);
    if (pos) n += (size_t)snprintf(out + n, cap - n, ",\"lat\":%.6f,\"lon\":%.6f", pos[0], pos[1]);
    n += (size_t)snprintf(out + n, cap - n, "}");
    return n;
}

static char s_line[1200];
static size_t s_line_len = 0;

static void print_decode(const Decoder& d) {
    const DecodeResult& r = d.res;
    Serial.printf("id=%s mfct=%s (%s) version=%02X type=%02X media=%s ci=%02X\n", r.id, r.mfct,
                  manufacturer_name(r.mfct_code) ? manufacturer_name(r.mfct_code) : "?", r.version, r.type, r.media,
                  r.ci);
    Serial.printf("driver=%s status=%s security=%s\n", r.driver ? r.driver->name : "-", decode_status_name(r.status),
                  decrypt_status_name(r.decrypt));
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& f = r.fields[i];
        if (f.hidden) continue;
        char v[96];
        field_format(&f, v, sizeof(v));
        Serial.printf("  %-36s %s\n", f.name, v);
    }
    format_json(d, 0, nullptr, nullptr, scratch(), SCRATCH_LEN);
    Serial.println(scratch());
}

void console_telegram(const Frame& f, const Decoder& d, int16_t rssi, const char* name) {
    SerialOut mode = (SerialOut)g_cfg.serial_out;
    const DecodeResult& r = d.res;
    switch (mode) {
    case SerialOut::Off: return;
    case SerialOut::Rtlwmbus:
        if (format_rtlwmbus(f, rssi, scratch(), SCRATCH_LEN)) Serial.println(scratch());
        return;
    case SerialOut::Json:
        if (r.status != DecodeStatus::Ok) return;
        format_json(d, rssi, name, nullptr, scratch(), SCRATCH_LEN);
        Serial.println(scratch());
        return;
    case SerialOut::Log: {
        char sum[40] = "";
        result_summary(r, sum, sizeof(sum));
        Serial.printf("[%s] %s %s %-10s %4ddBm %-12s %s%s%s\n", link_mode_name(f.mode), r.id, r.mfct,
                      r.driver ? r.driver->name : "-", rssi, decode_status_name(r.status), sum,
                      name ? "  " : "", name ? name : "");
        return;
    }
    }
}

void console_capture_error(const RadioCapture& c, CaptureStatus st) {
    if ((SerialOut)g_cfg.serial_out != SerialOut::Log) return;
    char hex[41];
    bytes_to_hex(c.data, c.len < 20 ? c.len : 20, hex, sizeof(hex));
    Serial.printf("[RF] %s len=%u %ddBm %s...\n", capture_status_name(st), c.len, c.rssi_dbm, hex);
}

static bool is_hex_line(const char* s) {
    size_t n = 0;
    for (const char* p = s; *p; ++p) {
        if (isxdigit((unsigned char)*p)) n++;
        else if (*p != ' ' && *p != '_' && *p != '|' && *p != '#') return false;
    }
    return n >= 20;
}

static void cmd_help() {
    Serial.println(
        "wM-Buster ADV commands:\n"
        "  <hex>                        analyze a telegram (radio capture or frame)\n"
        "  analyze <hex> [key] [driver] analyze with a key / forced driver\n"
        "  meters                       list meters heard\n"
        "  show <id>                    decode the last telegram of a meter\n"
        "  key <id> <32 hex|NONE>       set the AES key (16 hex digits = DES)\n"
        "  name <id> <text>             name a meter\n"
        "  driver <id> <name|auto>      force a driver\n"
        "  star <id>                    star/unstar a meter\n"
        "  forget <id>                  delete a meter configuration\n"
        "  import [path]                import keys (id,key,name,driver lines) from SD\n"
        "  band ct|s|hop                radio band (hop = alternate)\n"
        "  out off|log|json|rtlwmbus    serial telegram output\n"
        "  wifi <ssid> [pass] | wifi off   join a network\n"
        "  ap on|off                    access point\n"
        "  mqtt <host> [port] [user] [pass] | mqtt off\n"
        "  ntfy <url>|off               push notifications for starred meters\n"
        "  stats                        counters\n"
        "  reboot");
}

static void cmd_meters() {
    uint8_t idx[MAX_METERS];
    int n = meters_sorted(MeterSort::Recent, false, idx, MAX_METERS);
    Serial.printf("%d meters\n", n);
    for (int i = 0; i < n; ++i) {
        const Meter* m = meters_at(idx[i]);
        const MeterConf* c = meterconf_find(m->id);
        Serial.printf("%s %s %-10s %-4s %4ddBm %5lu  %-16s %s%s\n", m->id, m->mfct, m->driver ? m->driver->name : "-",
                      link_mode_name(m->mode), m->rssi, (unsigned long)m->count, m->summary,
                      decode_status_name(m->status), c && c->name[0] ? (String("  ") + c->name).c_str() : "");
    }
}

static MeterConf conf_for(const char* id) {
    const MeterConf* c = meterconf_find(id);
    MeterConf mc;
    if (c) mc = *c;
    else {
        memset(&mc, 0, sizeof(mc));
        snprintf(mc.id, sizeof(mc.id), "%s", id);
    }
    return mc;
}

static void save_conf(const MeterConf& c) {
    if (meterconf_put(c)) {
        app_meter_config_changed(c.id);
        Serial.println("ok");
    } else {
        Serial.println("error: invalid id or table full");
    }
}

static void cmd_stats() {
    RadioStats rs;
    radio_get_stats(&rs);
    Serial.printf("radio %s band %s: syncs %lu captures %lu noise %lu dropped %lu overflows %lu timeouts %lu floor %d dBm\n",
                  radio_chip_name(), radio_band_name(radio_band()), (unsigned long)rs.syncs, (unsigned long)rs.captures,
                  (unsigned long)rs.noise, (unsigned long)rs.dropped, (unsigned long)rs.overflows,
                  (unsigned long)rs.timeouts, rs.noise_floor);
    Serial.printf("frames %lu (C1 %lu T1 %lu S1 %lu) crc %lu coding %lu truncated %lu\n", (unsigned long)g_app.frames,
                  (unsigned long)g_app.c1, (unsigned long)g_app.t1, (unsigned long)g_app.s1,
                  (unsigned long)g_app.crc_errors, (unsigned long)g_app.bad_coding, (unsigned long)g_app.truncated);
    Serial.printf("decoded %lu encrypted %lu undecoded %lu, %lu/min, meters %d, heap %u (min %u)\n",
                  (unsigned long)g_app.decoded, (unsigned long)g_app.encrypted, (unsigned long)g_app.undecoded,
                  (unsigned long)g_app.per_min, meters_count(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
}

static void handle_line(char* line) {
    while (*line == ' ') line++;
    if (!*line) return;
    if (!strncmp(line, "--analyze=", 10)) line += 10;
    if (is_hex_line(line)) {
        bool ok;
        const Decoder* d = app_analyze(line, "", "", &ok);
        if (!d) Serial.println("error: not a wM-Bus / M-Bus frame (CRC?)");
        else print_decode(*d);
        return;
    }
    char* args[6] = { nullptr };
    int na = 0;
    char* save = nullptr;
    for (char* t = strtok_r(line, " ", &save); t && na < 6; t = strtok_r(nullptr, " ", &save)) {
        args[na++] = t;
        if (na == 2 && !strcmp(args[0], "name")) {
            // The rest of the line is the name.
            char* rest = save;
            while (rest && *rest == ' ') rest++;
            if (rest && *rest) args[na++] = rest;
            break;
        }
    }
    const char* cmd = args[0];
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmd_help();
    else if (!strcmp(cmd, "meters")) cmd_meters();
    else if (!strcmp(cmd, "stats")) cmd_stats();
    else if (!strcmp(cmd, "analyze") && na >= 2) {
        bool ok;
        const Decoder* d = app_analyze(args[1], na > 2 ? args[2] : "", na > 3 ? args[3] : "", &ok);
        if (!d) Serial.println("error: not a wM-Bus / M-Bus frame (CRC?)");
        else print_decode(*d);
    } else if (!strcmp(cmd, "show") && na >= 2) {
        const Decoder* d = app_redecode(meters_find(args[1]));
        if (!d) Serial.println("error: unknown meter");
        else print_decode(*d);
    } else if (!strcmp(cmd, "key") && na >= 3) {
        MeterConf c = conf_for(args[1]);
        if (!strcasecmp(args[2], "none") || !strcasecmp(args[2], "nokey")) c.key_len = 0;
        else if (!parse_key_hex(args[2], c.key, &c.key_len)) {
            Serial.println("error: key must be 32 (AES) or 16 (DES) hex digits");
            return;
        }
        save_conf(c);
    } else if (!strcmp(cmd, "name") && na >= 2) {
        MeterConf c = conf_for(args[1]);
        snprintf(c.name, sizeof(c.name), "%s", na > 2 ? args[2] : "");
        save_conf(c);
    } else if (!strcmp(cmd, "driver") && na >= 3) {
        MeterConf c = conf_for(args[1]);
        if (strcmp(args[2], "auto") != 0 && !driver_by_name(args[2])) {
            Serial.println("error: unknown driver");
            return;
        }
        snprintf(c.driver, sizeof(c.driver), "%s", strcmp(args[2], "auto") ? args[2] : "");
        save_conf(c);
    } else if (!strcmp(cmd, "star") && na >= 2) {
        MeterConf c = conf_for(args[1]);
        c.starred = !c.starred;
        save_conf(c);
    } else if (!strcmp(cmd, "forget") && na >= 2) {
        Serial.println(meterconf_del(args[1]) ? "ok" : "error: not configured");
    } else if (!strcmp(cmd, "import")) {
        int n = meterconf_import_sd(na > 1 ? args[1] : "/keys.txt");
        Serial.printf("imported %d meters\n", n);
    } else if (!strcmp(cmd, "band") && na >= 2) {
        if (!strcasecmp(args[1], "s") || !strcasecmp(args[1], "s1")) g_cfg.band_mode = (uint8_t)BandMode::S;
        else if (!strcasecmp(args[1], "hop")) g_cfg.band_mode = (uint8_t)BandMode::Hop;
        else g_cfg.band_mode = (uint8_t)BandMode::CT;
        settings_save();
        app_apply_radio_band();
        Serial.printf("band %s\n", app_band_label());
    } else if (!strcmp(cmd, "out") && na >= 2) {
        if (!strcmp(args[1], "off")) g_cfg.serial_out = (uint8_t)SerialOut::Off;
        else if (!strcmp(args[1], "json")) g_cfg.serial_out = (uint8_t)SerialOut::Json;
        else if (!strcmp(args[1], "rtlwmbus")) g_cfg.serial_out = (uint8_t)SerialOut::Rtlwmbus;
        else g_cfg.serial_out = (uint8_t)SerialOut::Log;
        settings_save();
    } else if (!strcmp(cmd, "wifi") && na >= 2) {
        if (!strcmp(args[1], "off")) {
            g_cfg.wifi_mode = (uint8_t)WifiMode::AP;
            g_cfg.wifi_ssid[0] = 0;
        } else {
            snprintf(g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), "%s", args[1]);
            snprintf(g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), "%s", na > 2 ? args[2] : "");
            g_cfg.wifi_mode = (uint8_t)WifiMode::APSTA;
        }
        settings_save();
        net_apply_settings();
    } else if (!strcmp(cmd, "ap") && na >= 2) {
        bool on = !strcmp(args[1], "on");
        bool sta = g_cfg.wifi_ssid[0] != 0;
        g_cfg.wifi_mode = (uint8_t)(on ? (sta ? WifiMode::APSTA : WifiMode::AP) : (sta ? WifiMode::STA : WifiMode::Off));
        settings_save();
        net_apply_settings();
    } else if (!strcmp(cmd, "mqtt") && na >= 2) {
        if (!strcmp(args[1], "off")) g_cfg.mqtt_enabled = false;
        else {
            g_cfg.mqtt_enabled = true;
            snprintf(g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), "%s", args[1]);
            g_cfg.mqtt_port = na > 2 ? (uint16_t)atoi(args[2]) : 1883;
            snprintf(g_cfg.mqtt_user, sizeof(g_cfg.mqtt_user), "%s", na > 3 ? args[3] : "");
            snprintf(g_cfg.mqtt_pass, sizeof(g_cfg.mqtt_pass), "%s", na > 4 ? args[4] : "");
        }
        settings_save();
        net_apply_settings();
    } else if (!strcmp(cmd, "ntfy") && na >= 2) {
        snprintf(g_cfg.ntfy_url, sizeof(g_cfg.ntfy_url), "%s", strcmp(args[1], "off") ? args[1] : "");
        settings_save();
    } else if (!strcmp(cmd, "reboot")) {
        Serial.println("rebooting");
        delay(100);
        ESP.restart();
    } else {
        Serial.println("unknown command, type help");
    }
}

void console_begin() {}

void console_loop() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\r' || c == '\n') {
            if (s_line_len) {
                s_line[s_line_len] = 0;
                handle_line(s_line);
                s_line_len = 0;
            }
            continue;
        }
        if (s_line_len + 1 < sizeof(s_line)) s_line[s_line_len++] = (char)c;
    }
}

} // namespace wmb
