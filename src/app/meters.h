// wM-Buster ADV — meters seen on air (decoded or not) and the live feed.
// Only the main (Arduino loop) task touches these.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/engine.h"

namespace wmb {

#define MAX_METERS 64
#define FEED_LEN   48

struct Meter {
    char          id[9];
    char          mfct[4];
    uint16_t      mfct_code;
    uint8_t       version;
    uint8_t       type;
    const DriverDef* driver;
    DecodeStatus  status;
    DecryptStatus decrypt;
    LinkMode      mode;
    int16_t       rssi;          // last
    int16_t       rssi_best;
    int32_t       rssi_sum;
    uint32_t      count;
    uint32_t      first_ms;
    uint32_t      last_ms;
    uint32_t      last_unix;     // 0 when the clock was not set
    uint32_t      interval_ms;   // smoothed spacing between telegrams
    char          summary[24];   // primary value, e.g. "123.456 m3"
    char          status_txt[32];// meter status field ("OK", "LEAK", ...)
    bool          alarm;         // status_txt is not OK
    bool          fix;           // position recorded at the best RSSI
    double        lat, lon;
    uint16_t      frame_len;     // last frame (CRCs removed), re-decoded on demand
    uint8_t       frame[WMB_FRAME_MAX];
    uint8_t       frame_mode;    // LinkMode
    uint8_t       frame_format;  // FrameFormat
};

struct FeedEntry {
    uint32_t     ms;
    char         id[9];
    char         mfct[4];
    int16_t      rssi;
    LinkMode     mode;
    DecodeStatus status;
    char         summary[24];
    const char*  driver;        // static driver name or nullptr
};

enum class MeterSort : uint8_t { Recent = 0, Signal = 1, Id = 2, Count = 3 };

// Update from a decode result; returns the meter (nullptr if the table is
// full of more important meters). *is_new is set for a first sighting.
// pos (lat, lon) is stored when this is the strongest reception so far.
Meter* meters_update(const Frame& f, const DecodeResult& r, int16_t rssi, const double* pos, bool* is_new);
Meter* meters_find(const char* id);
int    meters_count();
Meter* meters_at(int i);             // storage order
void   meters_clear();
// Indices sorted for display; returns count.
int    meters_sorted(MeterSort how, bool alarms_first, uint8_t* idx, int cap);

void   feed_add(const FeedEntry& e);
int    feed_count();
const FeedEntry* feed_at(int i);     // 0 = newest
void   feed_clear();

// Primary value / status text of a decode result (for lists and MQTT).
void result_summary(const DecodeResult& r, char* out, size_t cap);
void result_status(const DecodeResult& r, char* out, size_t cap);

} // namespace wmb
