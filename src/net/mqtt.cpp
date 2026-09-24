// wM-Buster ADV — MQTT publisher with Home Assistant discovery.
// GPL-3.0
#include "mqtt.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <string.h>

#include "../app/console.h"
#include "../app/settings.h"
#include "../app/scratch.h"

namespace wmb {

struct MqttMsg {
    char* topic;
    char* payload;
    bool retain;
};

struct MqttConf {
    bool enabled;
    char host[65];
    uint16_t port;
    char user[33];
    char pass[65];
    char prefix[33];
    uint32_t gen;
};

static MqttConf s_conf;
static SemaphoreHandle_t s_conf_mutex = nullptr;
static QueueHandle_t s_q = nullptr;
static volatile bool s_connected = false;
static volatile uint32_t s_published = 0;
static char s_error[48] = "";
static char s_client_id[32];

// Meters announced to Home Assistant this boot.
static char s_announced[64][9];
static int s_announced_n = 0;

static char* dup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void enqueue(const char* topic, const char* payload, bool retain) {
    if (!s_q || !s_conf.enabled) return;
    MqttMsg m{ dup(topic), dup(payload), retain };
    if (!m.topic || !m.payload || xQueueSend(s_q, &m, 0) != pdTRUE) {
        free(m.topic);
        free(m.payload);
    }
}

void mqtt_apply_settings() {
    if (!s_conf_mutex) return;
    xSemaphoreTake(s_conf_mutex, portMAX_DELAY);
    s_conf.enabled = g_cfg.mqtt_enabled && g_cfg.mqtt_host[0];
    snprintf(s_conf.host, sizeof(s_conf.host), "%s", g_cfg.mqtt_host);
    s_conf.port = g_cfg.mqtt_port ? g_cfg.mqtt_port : 1883;
    snprintf(s_conf.user, sizeof(s_conf.user), "%s", g_cfg.mqtt_user);
    snprintf(s_conf.pass, sizeof(s_conf.pass), "%s", g_cfg.mqtt_pass);
    snprintf(s_conf.prefix, sizeof(s_conf.prefix), "%s", g_cfg.mqtt_prefix[0] ? g_cfg.mqtt_prefix : "wmbusmeters");
    s_conf.gen++;
    xSemaphoreGive(s_conf_mutex);
    s_announced_n = 0;
}

bool mqtt_connected() { return s_connected; }
uint32_t mqtt_published() { return s_published; }
const char* mqtt_last_error() { return s_error; }

static void mqtt_task(void*) {
    WiFiClient net;
    PubSubClient client(net);
    client.setBufferSize(4096);
    client.setKeepAlive(30);
    client.setSocketTimeout(5);
    MqttConf conf;
    memset(&conf, 0, sizeof(conf));
    uint32_t next_try = 0, backoff = 2000;
    char will[80];
    for (;;) {
        xSemaphoreTake(s_conf_mutex, portMAX_DELAY);
        bool changed = conf.gen != s_conf.gen;
        if (changed) conf = s_conf;
        xSemaphoreGive(s_conf_mutex);
        if (changed && client.connected()) client.disconnect();

        if (!conf.enabled || WiFi.status() != WL_CONNECTED) {
            s_connected = false;
            MqttMsg m;
            while (xQueueReceive(s_q, &m, 0) == pdTRUE) {  // drop, nothing to send to
                free(m.topic);
                free(m.payload);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!client.connected()) {
            s_connected = false;
            if (millis() < next_try) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            client.setServer(conf.host, conf.port);
            snprintf(will, sizeof(will), "%s/wmbuster/status", conf.prefix);
            bool ok = client.connect(s_client_id, conf.user[0] ? conf.user : nullptr,
                                     conf.user[0] ? conf.pass : nullptr, will, 0, true, "offline");
            if (!ok) {
                snprintf(s_error, sizeof(s_error), "connect failed (state %d)", client.state());
                next_try = millis() + backoff;
                backoff = backoff < 60000 ? backoff * 2 : 60000;
                continue;
            }
            s_error[0] = 0;
            backoff = 2000;
            client.publish(will, "online", true);
            s_connected = true;
        }
        client.loop();
        MqttMsg m;
        int budget = 8;
        while (budget-- > 0 && xQueueReceive(s_q, &m, 0) == pdTRUE) {
            if (client.publish(m.topic, (const uint8_t*)m.payload, strlen(m.payload), m.retain)) s_published++;
            else snprintf(s_error, sizeof(s_error), "publish failed (%u bytes)", (unsigned)strlen(m.payload));
            free(m.topic);
            free(m.payload);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void mqtt_begin() {
    s_conf_mutex = xSemaphoreCreateMutex();
    s_q = xQueueCreate(24, sizeof(MqttMsg));
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_client_id, sizeof(s_client_id), "wmbuster-%02x%02x%02x", mac[3], mac[4], mac[5]);
    mqtt_apply_settings();
    xTaskCreatePinnedToCore(mqtt_task, "mqtt", 6144, nullptr, 2, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Home Assistant discovery
// ---------------------------------------------------------------------------
static void topic_safe(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = in; *p && o + 1 < cap; ++p) {
        char c = *p;
        out[o++] = (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    }
    out[o] = 0;
}

struct HaClass {
    const char* unit;
    const char* device_class;
    const char* state_class;
};

static HaClass ha_class(const OutField& f, const char* media) {
    bool total = strncmp(f.name, "total", 5) == 0;
    const char* inc = total ? "total_increasing" : "measurement";
    bool gas = media && strstr(media, "gas");
    switch (f.unit) {
    case Unit::M3: return { "m³", total ? (gas ? "gas" : "water") : nullptr, inc };
    case Unit::L: return { "L", total ? (gas ? "gas" : "water") : nullptr, inc };
    case Unit::KWH: return { "kWh", total ? "energy" : nullptr, inc };
    case Unit::WH: return { "Wh", total ? "energy" : nullptr, inc };
    case Unit::MJ: return { "MJ", total ? "energy" : nullptr, inc };
    case Unit::GJ: return { "GJ", total ? "energy" : nullptr, inc };
    case Unit::KW: return { "kW", "power", "measurement" };
    case Unit::W: return { "W", "power", "measurement" };
    case Unit::M3H: return { "m³/h", "volume_flow_rate", "measurement" };
    case Unit::LH: return { "L/h", nullptr, "measurement" };
    case Unit::C: return { "°C", "temperature", "measurement" };
    case Unit::K: return { "K", nullptr, "measurement" };
    case Unit::Volt: return { "V", "voltage", "measurement" };
    case Unit::Ampere: return { "A", "current", "measurement" };
    case Unit::HZ: return { "Hz", "frequency", "measurement" };
    case Unit::BAR: return { "bar", "pressure", "measurement" };
    case Unit::PA: return { "Pa", "pressure", "measurement" };
    case Unit::RH: return { "%", "humidity", "measurement" };
    case Unit::Second: return { "s", "duration", "measurement" };
    case Unit::Minute: return { "min", "duration", "measurement" };
    case Unit::Hour: return { "h", "duration", "measurement" };
    case Unit::Day: return { "d", "duration", "measurement" };
    case Unit::DBM: return { "dBm", "signal_strength", "measurement" };
    case Unit::PERCENTAGE: return { "%", nullptr, "measurement" };
    case Unit::HCA: return { nullptr, nullptr, total ? "total_increasing" : "measurement" };
    case Unit::DateLT: return { nullptr, "date", nullptr };
    default: break;
    }
    const char* u = unit_human(f.unit);
    return { u && u[0] ? u : nullptr, nullptr, f.is_text ? nullptr : "measurement" };
}

// Names go into json strings: keep them printable and quote free.
static void json_safe(char* s) {
    for (; *s; ++s)
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) *s = '\'';
}

static bool announced(const char* id) {
    for (int i = 0; i < s_announced_n; ++i)
        if (!strcmp(s_announced[i], id)) return true;
    return false;
}

void mqtt_rediscover(const char* id) {
    for (int i = 0; i < s_announced_n; ++i) {
        if (strcmp(s_announced[i], id) != 0) continue;
        memmove(s_announced[i], s_announced[i + 1], sizeof(s_announced[0]) * (size_t)(s_announced_n - i - 1));
        s_announced_n--;
        return;
    }
}

static void announce(const DecodeResult& r, const char* state_topic, const char* name) {
    static char payload[1024];
    static char topic[160];
    char dev_name[64];
    const char* mfct = manufacturer_name(r.mfct_code);
    if (name) snprintf(dev_name, sizeof(dev_name), "%s", name);
    else snprintf(dev_name, sizeof(dev_name), "%s %s %s", r.mfct, r.driver ? r.driver->name : "meter", r.id);
    json_safe(dev_name);
    char avty[80];
    snprintf(avty, sizeof(avty), "%s/wmbuster/status", s_conf.prefix);
    auto one = [&](const char* key, const char* label, const HaClass& c, bool text) {
        char obj[80];
        char safe[48];
        topic_safe(key, safe, sizeof(safe));
        snprintf(obj, sizeof(obj), "wmbus_%s_%s", r.id, safe);
        snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", obj);
        int o = snprintf(payload, sizeof(payload),
                         "{\"name\":\"%s\",\"uniq_id\":\"%s\",\"obj_id\":\"%s\",\"stat_t\":\"%s\","
                         "\"val_tpl\":\"{{ value_json.%s }}\",\"avty_t\":\"%s\"",
                         label, obj, obj, state_topic, key, avty);
        if (c.unit && !text) o += snprintf(payload + o, sizeof(payload) - o, ",\"unit_of_meas\":\"%s\"", c.unit);
        if (c.device_class) o += snprintf(payload + o, sizeof(payload) - o, ",\"dev_cla\":\"%s\"", c.device_class);
        if (c.state_class && !text) o += snprintf(payload + o, sizeof(payload) - o, ",\"stat_cla\":\"%s\"", c.state_class);
        if (!strcmp(key, "rssi_dbm")) o += snprintf(payload + o, sizeof(payload) - o, ",\"ent_cat\":\"diagnostic\"");
        snprintf(payload + o, sizeof(payload) - o,
                 ",\"dev\":{\"ids\":[\"wmbus_%s\"],\"name\":\"%s\",\"mf\":\"%s\",\"mdl\":\"%s\",\"via_device\":\"%s\"}}",
                 r.id, dev_name, mfct ? mfct : r.mfct, r.driver ? r.driver->name : "wM-Bus meter", s_client_id);
        enqueue(topic, payload, true);
    };
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& f = r.fields[i];
        if (f.hidden) continue;
        HaClass c = ha_class(f, r.media);
        char label[48];
        snprintf(label, sizeof(label), "%s", f.vname);
        for (char* p = label; *p; ++p) if (*p == '_') *p = ' ';
        json_safe(label);
        one(f.name, label, c, f.is_text);
    }
    one("rssi_dbm", "RSSI", HaClass{ "dBm", "signal_strength", "measurement" }, false);
}

void mqtt_publish_telegram(const Frame& f, const Decoder& d, int16_t rssi, const char* name) {
    if (!s_conf.enabled) return;
    const DecodeResult& r = d.res;
    char* payload = scratch();
    const size_t cap = SCRATCH_LEN;
    static char topic[128];
    char safe[40];
    if (r.status == DecodeStatus::Ok) {
        topic_safe(name ? name : r.id, safe, sizeof(safe));
        snprintf(topic, sizeof(topic), "%s/%s", s_conf.prefix, safe);
        format_json(d, rssi, name, nullptr, payload, cap);
        enqueue(topic, payload, false);
        // Discovery for configured meters only: announcing every neighbour's
        // meter would flood Home Assistant with devices.
        if (g_cfg.mqtt_ha && meterconf_find(r.id) && !announced(r.id) && s_announced_n < 64) {
            announce(r, topic, name);
            snprintf(s_announced[s_announced_n++], 9, "%s", r.id);
        }
    } else if (g_cfg.mqtt_raw) {
        snprintf(topic, sizeof(topic), "%s/raw/%s", s_conf.prefix, r.id[0] ? r.id : "unknown");
        int o = snprintf(payload, cap, "{\"id\":\"%s\",\"mfct\":\"%s\",\"status\":\"%s\",\"rssi_dbm\":%d,\"hex\":\"",
                         r.id, r.mfct, decode_status_name(r.status), rssi);
        bytes_to_hex(f.data, f.len, payload + o, cap - (size_t)o - 4);
        strcat(payload, "\"}");
        enqueue(topic, payload, false);
    }
}

void mqtt_publish_state(const char* json) {
    if (!s_conf.enabled) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/wmbuster/state", s_conf.prefix);
    enqueue(topic, json, false);
}

} // namespace wmb
