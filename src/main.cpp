// wM-Buster ADV — wireless M-Bus receiver/decoder firmware for the M5Stack
// Cardputer-ADV with the Cap LoRa-1262 (SX1262) or Hydra RF (CC1101) cap.
// GPL-3.0
#include <Arduino.h>
#include <M5Cardputer.h>
#include <time.h>

#include "config.h"
#include "version.h"
#include "radio/radio.h"
#include "app/app.h"
#include "app/console.h"
#include "app/sdlog.h"
#include "app/settings.h"
#include "hw/board.h"
#include "hw/gnss.h"
#include "net/mqtt.h"
#include "net/net.h"
#include "net/ntfy.h"
#include "ui/ui.h"

// JSON rendering and the web handlers need more than the default 8 kB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

using namespace wmb;

static void publish_state() {
    static uint32_t last = 0;
    if (millis() - last < 60000) return;
    last = millis();
    RadioStats rs;
    radio_get_stats(&rs);
    char js[320];
    snprintf(js, sizeof(js),
             "{\"version\":\"%s\",\"uptime\":%lu,\"radio\":\"%s\",\"band\":\"%s\",\"frames\":%lu,\"decoded\":%lu,"
             "\"encrypted\":%lu,\"crc_errors\":%lu,\"per_min\":%lu,\"meters\":%d,\"noise_floor\":%d,\"heap\":%u}",
             WMB_VERSION, (unsigned long)(millis() / 1000), radio_chip_name(), radio_band_name(radio_band()),
             (unsigned long)g_app.frames, (unsigned long)g_app.decoded, (unsigned long)g_app.encrypted,
             (unsigned long)g_app.crc_errors, (unsigned long)g_app.per_min, meters_count(), rs.noise_floor,
             ESP.getFreeHeap());
    mqtt_publish_state(js);
}

void setup() {
    Serial.begin(115200);
    setenv("TZ", "UTC0", 1);
    tzset();
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.printf("\n=== wM-Buster ADV %s ===\n", WMB_VERSION);

    settings_load();
    meterconf_load();
    board_begin();
    ui_begin();
    ui_splash("Starting...");

    // The SD card shares the SPI bus with the radio: mount it first.
    if (sdlog_begin()) {
        int n = meterconf_import_sd("/keys.txt");
        if (n) Serial.printf("[KEYS] %d meters from /keys.txt\n", n);
    }
    app_begin();
    console_begin();

    ui_splash("Detecting radio...");
    RadioOptions ro;
    ro.hw = (RadioHw)g_cfg.radio_hw;
    ro.band = g_cfg.band_mode == (uint8_t)BandMode::S ? RadioBand::S : RadioBand::CT;
    ro.sx_rx_bw_khz = settings_sx_bw(g_cfg.sx_bw_idx);
    ro.sx_boosted_gain = g_cfg.boosted_gain;
    ro.rf_switch = board_rf_switch;
    bool radio_ok = radio_begin(ro);
    // The Hydra RF cap uses G13 (GNSS TX on the LoRa cap) as chip select.
    if (radio_chip() != RadioChip::CC1101) gnss_begin();

    char msg[64];
    if (radio_ok) snprintf(msg, sizeof(msg), "%s ready - %s", radio_chip_name(), app_band_label());
    else snprintf(msg, sizeof(msg), "No radio cap found");
    ui_splash(msg);
    Serial.printf("[MAIN] %s, %d meters configured\n", msg, meterconf_count());

    net_begin();
    ntfy_begin();
    delay(600);
    board_user_activity();
}

void loop() {
    M5Cardputer.update();
    app_loop();
    console_loop();
    gnss_loop();
    sdlog_loop();
    net_loop();
    board_loop();
    ui_loop();
    publish_state();
    delay(1);
}
