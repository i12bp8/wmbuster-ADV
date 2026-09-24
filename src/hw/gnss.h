// wM-Buster ADV — GNSS on the Cap LoRa-1262 (AT6668 / ATGM336H class).
// Auto-detects 115200/9600 baud, provides the position for wardriving logs
// and sets the system clock when no network time is available.
// GPL-3.0
#pragma once

#include <stdint.h>

namespace wmb {

struct GnssInfo {
    bool     present;     // NMEA sentences are coming in
    bool     fix;
    double   lat, lon;
    float    alt_m;
    float    hdop;
    uint8_t  sats;
    uint32_t baud;
};

void gnss_begin();
void gnss_end();              // release the UART pins (CC1101 cap uses G13)
void gnss_loop();
bool gnss_position(double* lat, double* lon);
void gnss_info(GnssInfo* out);

} // namespace wmb
