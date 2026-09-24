// wM-Buster ADV — MQTT publisher (own task, never blocks the UI/radio loop).
//
//   <prefix>/<name|id>          wmbusmeters compatible json per telegram
//   <prefix>/raw/<id>           undecoded telegrams as hex (optional)
//   <prefix>/wmbuster/status    online / offline (retained, last will)
//   <prefix>/wmbuster/state     device statistics every minute
//   homeassistant/sensor/...    Home Assistant discovery for configured meters
// GPL-3.0
#pragma once

#include <stdint.h>
#include "wmbus/engine.h"

namespace wmb {

void mqtt_begin();
void mqtt_apply_settings();
bool mqtt_connected();
uint32_t mqtt_published();
const char* mqtt_last_error();

// Called by the pipeline for every telegram (loop task).
void mqtt_publish_telegram(const Frame& f, const Decoder& d, int16_t rssi, const char* name);
// Called about once a minute with a device state json.
void mqtt_publish_state(const char* json);
// Forget which meters were announced to Home Assistant (config changed).
void mqtt_rediscover(const char* id);

} // namespace wmb
