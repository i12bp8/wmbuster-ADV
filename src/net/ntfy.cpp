// wM-Buster ADV — ntfy.sh push notifications.
// GPL-3.0
#include "ntfy.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "../app/settings.h"

namespace wmb {

struct NtfyMsg {
    char title[48];
    char message[128];
};

static QueueHandle_t s_q = nullptr;

static void ntfy_task(void*) {
    NtfyMsg m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        char url[sizeof(g_cfg.ntfy_url)];
        snprintf(url, sizeof(url), "%s", g_cfg.ntfy_url);
        if (!url[0] || WiFi.status() != WL_CONNECTED) continue;
        HTTPClient http;
        WiFiClientSecure tls;
        WiFiClient plain;
        bool ok;
        if (!strncmp(url, "https://", 8)) {
            tls.setInsecure();  // no CA bundle on the device
            ok = http.begin(tls, url);
        } else {
            ok = http.begin(plain, url);
        }
        if (!ok) continue;
        http.setTimeout(5000);
        http.addHeader("Title", m.title);
        http.addHeader("Tags", "droplet");
        int code = http.POST((uint8_t*)m.message, strlen(m.message));
        if (code < 200 || code >= 300) Serial.printf("[NTFY] POST failed: %d\n", code);
        http.end();
    }
}

void ntfy_begin() {
    s_q = xQueueCreate(6, sizeof(NtfyMsg));
    xTaskCreatePinnedToCore(ntfy_task, "ntfy", 6144, nullptr, 1, nullptr, 0);
}

void ntfy_notify(const char* title, const char* message) {
    if (!s_q || !g_cfg.ntfy_url[0]) return;
    NtfyMsg m;
    snprintf(m.title, sizeof(m.title), "%s", title);
    snprintf(m.message, sizeof(m.message), "%s", message);
    xQueueSend(s_q, &m, 0);
}

} // namespace wmb
