// wM-Buster ADV — persistent settings and per-meter configuration (NVS).
// GPL-3.0
#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <ctype.h>
#include <string.h>

namespace wmb {

Settings g_cfg;

static const uint16_t SETTINGS_VERSION = 3;
static const float SX_BW[] = { 156.2f, 187.2f, 234.3f, 312.0f, 373.6f, 467.0f };

float settings_sx_bw(uint8_t idx) { return SX_BW[idx < sizeof(SX_BW) / sizeof(SX_BW[0]) ? idx : 2]; }
uint8_t settings_sx_bw_count() { return (uint8_t)(sizeof(SX_BW) / sizeof(SX_BW[0])); }

static void copy(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src ? src : ""); }

void settings_defaults(Settings* s) {
    memset(s, 0, sizeof(*s));
    s->version = SETTINGS_VERSION;
    s->radio_hw = 0;
    s->band_mode = (uint8_t)BandMode::CT;
    s->hop_s = 30;
    s->sx_bw_idx = 2;
    s->boosted_gain = true;
    s->wifi_mode = (uint8_t)WifiMode::AP;
    copy(s->ap_pass, sizeof(s->ap_pass), "wmbuster");
    copy(s->hostname, sizeof(s->hostname), "wmbuster");
    s->mqtt_port = 1883;
    copy(s->mqtt_prefix, sizeof(s->mqtt_prefix), "wmbusmeters");
    s->mqtt_ha = true;
    s->sd_log = true;
    s->serial_out = (uint8_t)SerialOut::Log;
    s->brightness = 140;
    s->dim_s = 90;
    s->sound = true;
}

void settings_load() {
    Preferences p;
    settings_defaults(&g_cfg);
    if (!p.begin("wmbadv", true)) return;
    Settings tmp;
    size_t n = p.getBytes("settings", &tmp, sizeof(tmp));
    p.end();
    if (n == sizeof(tmp) && tmp.version == SETTINGS_VERSION) {
        g_cfg = tmp;
        // Always keep strings terminated.
        g_cfg.wifi_ssid[sizeof(g_cfg.wifi_ssid) - 1] = 0;
        g_cfg.wifi_pass[sizeof(g_cfg.wifi_pass) - 1] = 0;
        g_cfg.ap_ssid[sizeof(g_cfg.ap_ssid) - 1] = 0;
        g_cfg.ap_pass[sizeof(g_cfg.ap_pass) - 1] = 0;
        g_cfg.hostname[sizeof(g_cfg.hostname) - 1] = 0;
        g_cfg.web_pass[sizeof(g_cfg.web_pass) - 1] = 0;
        g_cfg.mqtt_host[sizeof(g_cfg.mqtt_host) - 1] = 0;
        g_cfg.mqtt_user[sizeof(g_cfg.mqtt_user) - 1] = 0;
        g_cfg.mqtt_pass[sizeof(g_cfg.mqtt_pass) - 1] = 0;
        g_cfg.mqtt_prefix[sizeof(g_cfg.mqtt_prefix) - 1] = 0;
        g_cfg.ntfy_url[sizeof(g_cfg.ntfy_url) - 1] = 0;
    }
    if (g_cfg.brightness < 10) g_cfg.brightness = 10;
}

void settings_save() {
    Preferences p;
    if (!p.begin("wmbadv", false)) return;
    g_cfg.version = SETTINGS_VERSION;
    p.putBytes("settings", &g_cfg, sizeof(g_cfg));
    p.end();
}

// ---------------------------------------------------------------------------
// Meter configuration
// ---------------------------------------------------------------------------
static MeterConf s_mc[MAX_METER_CONF];
static int s_mc_n = 0;

static void meterconf_save() {
    Preferences p;
    if (!p.begin("wmbadv", false)) return;
    if (s_mc_n == 0) p.remove("meters");
    else p.putBytes("meters", s_mc, sizeof(MeterConf) * (size_t)s_mc_n);
    p.end();
}

void meterconf_load() {
    s_mc_n = 0;
    Preferences p;
    if (!p.begin("wmbadv", true)) return;
    size_t n = p.getBytesLength("meters");
    if (n > 0 && n % sizeof(MeterConf) == 0 && n <= sizeof(s_mc)) {
        p.getBytes("meters", s_mc, n);
        s_mc_n = (int)(n / sizeof(MeterConf));
    }
    p.end();
    for (int i = 0; i < s_mc_n; ++i) {
        s_mc[i].id[8] = 0;
        s_mc[i].name[sizeof(s_mc[i].name) - 1] = 0;
        s_mc[i].driver[sizeof(s_mc[i].driver) - 1] = 0;
    }
}

const MeterConf* meterconf_find(const char* id) {
    for (int i = 0; i < s_mc_n; ++i)
        if (!strcasecmp(s_mc[i].id, id)) return &s_mc[i];
    return nullptr;
}

// Updates the RAM table; *changed tells whether anything differs.
static bool put_nosave(const MeterConf& c, bool* changed) {
    *changed = false;
    if (!meter_id_valid(c.id)) return false;
    MeterConf n = c;
    for (char* p = n.id; *p; ++p) *p = (char)tolower((unsigned char)*p);
    if (!n.key_len) memset(n.key, 0, sizeof(n.key));
    MeterConf* slot = nullptr;
    for (int i = 0; i < s_mc_n; ++i)
        if (!strcasecmp(s_mc[i].id, n.id)) slot = &s_mc[i];
    if (!slot) {
        if (s_mc_n >= MAX_METER_CONF) return false;
        slot = &s_mc[s_mc_n++];
        memset(slot, 0, sizeof(*slot));
        *changed = true;
    }
    if (memcmp(slot, &n, sizeof(n)) != 0) *changed = true;
    *slot = n;
    return true;
}

bool meterconf_put(const MeterConf& c) {
    bool changed;
    if (!put_nosave(c, &changed)) return false;
    if (changed) meterconf_save();
    return true;
}

bool meterconf_del(const char* id) {
    for (int i = 0; i < s_mc_n; ++i) {
        if (strcasecmp(s_mc[i].id, id) != 0) continue;
        memmove(&s_mc[i], &s_mc[i + 1], sizeof(MeterConf) * (size_t)(s_mc_n - i - 1));
        s_mc_n--;
        meterconf_save();
        return true;
    }
    return false;
}

int meterconf_count() { return s_mc_n; }
const MeterConf* meterconf_at(int i) { return (i >= 0 && i < s_mc_n) ? &s_mc[i] : nullptr; }

bool meter_id_valid(const char* id) {
    if (!id || strlen(id) != 8) return false;
    for (int i = 0; i < 8; ++i)
        if (!isxdigit((unsigned char)id[i])) return false;
    return true;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool parse_key_hex(const char* hex, uint8_t* out, uint8_t* len) {
    uint8_t buf[16];
    size_t n = 0;
    int hi = -1;
    for (const char* p = hex; *p; ++p) {
        if (*p == ' ' || *p == ':' || *p == '-' || *p == '_') continue;
        int v = hexv(*p);
        if (v < 0) return false;
        if (hi < 0) { hi = v; continue; }
        if (n >= sizeof(buf)) return false;
        buf[n++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    if (hi >= 0 || (n != 16 && n != 8)) return false;
    memcpy(out, buf, n);
    *len = (uint8_t)n;
    return true;
}

void key_to_hex(const uint8_t* key, uint8_t len, char* out, size_t cap) {
    static const char H[] = "0123456789ABCDEF";
    size_t o = 0;
    for (uint8_t i = 0; i < len && o + 2 < cap; ++i) {
        out[o++] = H[key[i] >> 4];
        out[o++] = H[key[i] & 15];
    }
    if (cap) out[o] = 0;
}

static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool s_import_changed = false;

static bool import_put(const MeterConf& c) {
    bool changed;
    if (!put_nosave(c, &changed)) return false;
    s_import_changed |= changed;
    return true;
}

// One meter from wmbusmeters style "key=value" lines (a meter file).
static bool import_kv_block(char* block) {
    MeterConf c;
    memset(&c, 0, sizeof(c));
    char* save = nullptr;
    for (char* line = strtok_r(block, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = trim(line);
        char* v = trim(eq + 1);
        if (!strcmp(k, "id")) copy(c.id, sizeof(c.id), v);
        else if (!strcmp(k, "name")) copy(c.name, sizeof(c.name), v);
        else if (!strcmp(k, "driver")) copy(c.driver, sizeof(c.driver), strcmp(v, "auto") ? v : "");
        else if (!strcmp(k, "key") && strcmp(v, "NOKEY") != 0) parse_key_hex(v, c.key, &c.key_len);
    }
    if (!meter_id_valid(c.id)) return false;
    const MeterConf* old = meterconf_find(c.id);
    if (old) c.starred = old->starred;
    return import_put(c);
}

int meterconf_import_text(const char* text) {
    int n = 0;
    s_import_changed = false;
    size_t len = strlen(text);
    char* buf = (char*)malloc(len + 1);
    if (!buf) return 0;
    memcpy(buf, text, len + 1);
    if (strstr(buf, "id=") || strstr(buf, "id =")) {
        // Blocks separated by blank lines.
        char* p = buf;
        while (*p) {
            char* end = strstr(p, "\n\n");
            if (end) *end = 0;
            if (import_kv_block(p)) n++;
            if (!end) break;
            p = end + 2;
        }
    } else {
        char* save = nullptr;
        for (char* line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save)) {
            line = trim(line);
            if (!*line || *line == '#') continue;
            char* parts[4] = { nullptr, nullptr, nullptr, nullptr };
            int np = 0;
            char* s2 = nullptr;
            for (char* t = strtok_r(line, ",;\t ", &s2); t && np < 4; t = strtok_r(nullptr, ",;\t ", &s2)) parts[np++] = t;
            if (np < 2 || !meter_id_valid(parts[0])) continue;
            MeterConf c;
            memset(&c, 0, sizeof(c));
            copy(c.id, sizeof(c.id), parts[0]);
            if (strcmp(parts[1], "NOKEY") != 0 && !parse_key_hex(parts[1], c.key, &c.key_len)) continue;
            if (np > 2) copy(c.name, sizeof(c.name), parts[2]);
            if (np > 3 && strcmp(parts[3], "auto") != 0) copy(c.driver, sizeof(c.driver), parts[3]);
            const MeterConf* old = meterconf_find(c.id);
            if (old) {
                c.starred = old->starred;
                if (!c.name[0]) copy(c.name, sizeof(c.name), old->name);
            }
            if (import_put(c)) n++;
        }
    }
    free(buf);
    if (s_import_changed) meterconf_save();
    s_import_changed = false;
    return n;
}

int meterconf_import_sd(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    size_t sz = f.size();
    if (sz == 0 || sz > 16384) {
        f.close();
        return 0;
    }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        f.close();
        return 0;
    }
    size_t rd = f.read((uint8_t*)buf, sz);
    f.close();
    buf[rd] = 0;
    for (size_t i = 0; i < rd; ++i) if (buf[i] == '\r') buf[i] = '\n';
    int n = meterconf_import_text(buf);
    free(buf);
    return n;
}

} // namespace wmb
