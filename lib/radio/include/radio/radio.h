// wM-Buster ADV — wM-Bus radio front end for the Cardputer-ADV caps.
//
// Supported transceivers:
//   * SX1262 (M5 Cap LoRa-1262): RadioLib for configuration, then a sync
//     word interrupt starts a timed read of the RX buffer. The frame length
//     is read from the first bytes so reception stops right after the frame
//     and false syncs on noise are dropped within half a millisecond.
//   * CC1101 (Hydra RF cap): raw SPI, infinite packet mode, the RX FIFO is
//     streamed while the frame arrives (no 64 byte limit).
//
// A dedicated high priority task owns the radio and hands complete raw
// captures (the bytes after the 16 bit sync word) to the application through
// a queue. frame_from_capture() turns them into frames.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/frame.h"

namespace wmb {

#define RADIO_CAPTURE_MAX 512

enum class RadioChip : uint8_t { None = 0, SX1262, CC1101 };

// Requested hardware: auto detect, or force one of the caps.
enum class RadioHw : uint8_t { Auto = 0, SX1262 = 1, CC1101 = 2 };

struct RadioCapture {
    uint8_t   data[RADIO_CAPTURE_MAX]; // raw bytes following the sync word
    uint16_t  len;
    int16_t   rssi_dbm;                // measured right after sync detection
    RadioBand band;
    uint32_t  t_ms;                    // millis() at sync detection
};

struct RadioStats {
    uint32_t syncs;        // sync word detections
    uint32_t captures;     // captures handed to the application
    uint32_t noise;        // syncs whose header was not a wM-Bus frame
    uint32_t dropped;      // capture queue was full
    uint32_t overflows;    // RX FIFO overflows (CC1101)
    uint32_t timeouts;     // frame did not complete in time
    uint32_t restarts;     // receiver re-arms by the watchdog
    int16_t  noise_floor;  // dBm, sampled while idle
    uint16_t max_len;      // longest capture so far
};

// Optional hook that enables the antenna path before the SX1262 is
// initialised (the Cap LoRa-1262 RF switch sits on an I2C expander).
typedef bool (*RadioRfSwitchFn)();

struct RadioOptions {
    RadioHw   hw = RadioHw::Auto;
    RadioBand band = RadioBand::CT;
    float     sx_rx_bw_khz = 234.3f;   // SX1262 C/T band receiver bandwidth
    bool      sx_boosted_gain = true;   // SX1262 RX boosted gain (+~2 dB)
    RadioRfSwitchFn rf_switch = nullptr;
};

// Detects the cap, configures it and starts the receiver task.
bool radio_begin(const RadioOptions& opt);
RadioChip   radio_chip();
const char* radio_chip_name();
bool        radio_ready();

// Band changes are executed by the radio task (safe from any task).
void      radio_set_band(RadioBand band);
RadioBand radio_band();
const char* radio_band_name(RadioBand band);

// Next capture (waits up to wait_ms). Returns false when none arrived.
bool radio_receive(RadioCapture* out, uint32_t wait_ms);

void radio_get_stats(RadioStats* out);

// True while a frame is being received: callers that share the SPI bus (SD
// card) should defer long transfers.
bool radio_busy();

} // namespace wmb
