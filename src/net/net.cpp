// wM-Buster ADV — WiFi access point / station, mDNS and NTP.
// GPL-3.0
#include "net.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "mqtt.h"
#include "../app/settings.h"

namespace wmb {

static bool s_ap = false;
static bool s_sta = false;
static bool s_mdns = false;
static char s_ap_ssid[33];
static uint32_t s_sta_since = 0;
static uint32_t s_sta_retry = 0;
static bool s_was_connected = false;

static void default_ap_ssid(char* out, size_t cap) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(out, cap, "wM-Buster-%02X%02X", mac[4], mac[5]);
}

void net_apply_settings() {
    WifiMode m = (WifiMode)g_cfg.wifi_mode;
    bool ap = m == WifiMode::AP || m == WifiMode::APSTA;
    bool sta = (m == WifiMode::STA || m == WifiMode::APSTA) && g_cfg.wifi_ssid[0];
    // Without a network to join, keep the AP up so the device stays reachable.
    if (m == WifiMode::STA && !g_cfg.wifi_ssid[0]) ap = true;
    if (!ap && !sta) {
        if (s_mdns) MDNS.end();
        s_mdns = false;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        s_ap = s_sta = false;
        mqtt_apply_settings();
        return;
    }
    WiFi.mode(ap && sta ? WIFI_AP_STA : ap ? WIFI_AP : WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname(g_cfg.hostname[0] ? g_cfg.hostname : "wmbuster");
    if (ap) {
        if (g_cfg.ap_ssid[0]) snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", g_cfg.ap_ssid);
        else default_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));
        const char* pass = strlen(g_cfg.ap_pass) >= 8 ? g_cfg.ap_pass : nullptr;
        WiFi.softAP(s_ap_ssid, pass);
    } else if (s_ap) {
        WiFi.softAPdisconnect(true);
    }
    if (sta) {
        WiFi.disconnect();
        WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
        s_sta_since = millis();
        s_sta_retry = 0;
    } else if (s_sta) {
        WiFi.disconnect();
    }
    s_ap = ap;
    s_sta = sta;
    if (!s_mdns) s_mdns = MDNS.begin(g_cfg.hostname[0] ? g_cfg.hostname : "wmbuster");
    if (s_mdns) MDNS.addService("http", "tcp", 80);
    mqtt_apply_settings();
}

void net_begin() {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    net_apply_settings();
    web_begin();
    mqtt_begin();
}

void net_loop() {
    if (s_sta) {
        bool conn = WiFi.status() == WL_CONNECTED;
        if (conn && !s_was_connected) {
            Serial.printf("[WIFI] connected to %s, IP %s\n", g_cfg.wifi_ssid, WiFi.localIP().toString().c_str());
            configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        }
        if (!conn && millis() - s_sta_since > 20000 && millis() - s_sta_retry > 30000) {
            // Auto reconnect gave up (router rebooted, wrong channel...): try again.
            s_sta_retry = millis();
            WiFi.disconnect();
            WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
        }
        s_was_connected = conn;
    }
    web_loop();
}

void net_status(NetStatus* o) {
    memset(o, 0, sizeof(*o));
    o->ap_on = s_ap;
    snprintf(o->ap_ssid, sizeof(o->ap_ssid), "%s", s_ap ? s_ap_ssid : "");
    if (s_ap) {
        snprintf(o->ap_ip, sizeof(o->ap_ip), "%s", WiFi.softAPIP().toString().c_str());
        o->ap_clients = WiFi.softAPgetStationNum();
    }
    o->sta_connected = s_sta && WiFi.status() == WL_CONNECTED;
    if (o->sta_connected) {
        snprintf(o->sta_ip, sizeof(o->sta_ip), "%s", WiFi.localIP().toString().c_str());
        o->sta_rssi = WiFi.RSSI();
    }
    snprintf(o->sta_ssid, sizeof(o->sta_ssid), "%s", s_sta ? g_cfg.wifi_ssid : "");
    o->mqtt_connected = mqtt_connected();
    o->time_synced = time(nullptr) > 1600000000;
}

} // namespace wmb
