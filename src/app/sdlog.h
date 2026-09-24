// wM-Buster ADV — SD card logging.
//   /wmbuster/telegrams.rtl   rtl_wmbus lines, replay with
//                             wmbusmeters stdin:rtlwmbus < telegrams.rtl
//   /wmbuster/telegrams.jsonl decoded telegrams (wmbusmeters json + rssi/position)
//   /wmbuster/wardrive.csv    one line per telegram with a GNSS position
// Lines are buffered in RAM and written while the radio is idle, since the
// card shares the SPI bus with the transceiver.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/engine.h"

namespace wmb {

bool sdlog_begin();                 // mounts the card (call before radio_begin)
bool sdlog_card_ok();
uint64_t sdlog_card_free();         // bytes, 0 when unknown
void sdlog_loop();
void sdlog_telegram(const Frame& f, const Decoder& d, int16_t rssi, const double* pos);
uint32_t sdlog_lines();             // lines written this boot
uint32_t sdlog_dropped();           // lines lost (buffer full / no card)

} // namespace wmb
