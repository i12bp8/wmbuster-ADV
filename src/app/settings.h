// wM-Buster ADV — persistent settings and per-meter configuration (NVS).
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

enum class BandMode : uint8_t { CT = 0, S = 1, Hop = 2 };
enum class WifiMode : uint8_t { Off = 0, AP = 1, STA = 2, APSTA = 3 };
enum class SerialOut : uint8_t { Off = 0, Log = 1, Json = 2, Rtlwmbus = 3 };

struct Settings {
    uint16_t version;
    // Radio
    uint8_t  radio_hw;        // RadioHw: 0 auto, 1 SX1262, 2 CC1101
    uint8_t  band_mode;       // BandMode
    uint16_t hop_s;           // dwell time per band when hopping
    uint8_t  sx_bw_idx;       // index into settings_sx_bw()
    bool     boosted_gain;
    // Network
    uint8_t  wifi_mode;       // WifiMode
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     ap_ssid[33];
    char     ap_pass[65];
    char     hostname[32];
    char     web_pass[33];    // optional HTTP basic auth password (user "admin")
    // MQTT
    bool     mqtt_enabled;
    char     mqtt_host[65];
    uint16_t mqtt_port;
    char     mqtt_user[33];
    char     mqtt_pass[65];
    char     mqtt_prefix[33];
    bool     mqtt_ha;         // Home Assistant discovery
    bool     mqtt_raw;        // also publish undecoded telegrams (hex)
    // Notifications
    char     ntfy_url[97];
    // Logging / outputs
    bool     sd_log;
    uint8_t  serial_out;      // SerialOut
    // UI
    uint8_t  theme;
    uint8_t  brightness;      // 10..255
    uint16_t dim_s;           // 0 = never
    bool     sound;
    bool     beep_all;        // beep on every telegram, not just new meters
    int16_t  tz_min;          // display offset from UTC in minutes
};

extern Settings g_cfg;

void settings_defaults(Settings* s);
void settings_load();
void settings_save();
// SX1262 receiver bandwidths offered in the settings (kHz).
float settings_sx_bw(uint8_t idx);
uint8_t settings_sx_bw_count();

// ---------------------------------------------------------------------------
// Per meter configuration: name, key (AES-128, or 8 byte DES), driver.
// ---------------------------------------------------------------------------
#define MAX_METER_CONF 64

struct MeterConf {
    char    id[9];
    char    name[24];
    uint8_t key[16];
    uint8_t key_len;          // 0 = none, 16 = AES, 8 = DES
    char    driver[20];       // "" = auto detect
    bool    starred;
};

void meterconf_load();
const MeterConf* meterconf_find(const char* id);
bool meterconf_put(const MeterConf& c);   // insert or replace, persists
bool meterconf_del(const char* id);
int  meterconf_count();
const MeterConf* meterconf_at(int i);
// Import "id,key[,name[,driver]]" lines (also accepts ';' and whitespace).
// Returns the number of meters imported.
int  meterconf_import_text(const char* text);
int  meterconf_import_sd(const char* path);

bool parse_key_hex(const char* hex, uint8_t* out, uint8_t* len); // 32 or 16 hex digits
void key_to_hex(const uint8_t* key, uint8_t len, char* out, size_t cap);
bool meter_id_valid(const char* id);

} // namespace wmb
