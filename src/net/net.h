// wM-Buster ADV — networking: WiFi (AP / station), mDNS, NTP, web UI + API.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

void net_begin();
void net_loop();
void net_apply_settings();          // WiFi/MQTT settings changed

struct NetStatus {
    bool ap_on;
    bool sta_connected;
    char ap_ssid[33];
    char ap_ip[16];
    char sta_ip[16];
    char sta_ssid[33];
    int  sta_rssi;
    int  ap_clients;
    bool mqtt_connected;
    bool time_synced;
};
void net_status(NetStatus* out);

// Web server (web.cpp)
void web_begin();
void web_loop();

} // namespace wmb
