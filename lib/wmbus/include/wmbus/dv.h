// wM-Buster ADV — DIF/VIF data record parsing (EN 13757-3), port of the
// wmbusmeters dvparser semantics: difvif keys (with _2, _3 suffixes for
// repeats), storage/tariff/subunit numbers, VIF combinables, manufacturer data,
// compact frames with format signatures, and value extraction helpers.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/types.h"

namespace wmb {

#ifndef WMB_MAX_DV
#define WMB_MAX_DV 112
#endif
#define WMB_DV_KEY_MAX 44
#define WMB_DV_MAX_COMB 4
#define WMB_DV_ARENA 1024

struct DvEntry {
    char     key[WMB_DV_KEY_MAX];   // "0C13", "02FF20_2", ...
    uint16_t offset;                // offset in telegram buffer (ordering)
    uint8_t  dif;                   // first DIF byte
    uint16_t vif;                   // full VIF (0x7Bxx / 0x7Dxx for extensions)
    MeasurementType mtype;
    uint32_t storage;
    uint16_t tariff;
    uint16_t subunit;
    uint8_t  ncomb;
    uint16_t comb_raw[WMB_DV_MAX_COMB];
    const uint8_t* data;            // value bytes (in telegram buffer or arena)
    uint8_t  data_len;
    bool     synthetic;             // produced by ixml / compact expansion
    bool     named_combs_only;      // compact profile point: no raw combinables
};

struct DvSet {
    DvEntry  e[WMB_MAX_DV];
    int      n;
    uint8_t  arena[WMB_DV_ARENA];
    size_t   arena_used;
    int      mfct_0f_index;        // index (in payload) of mfct data after 0x0F, -1 when none
    uint16_t format_hash;          // crc16 of the difvif format bytes (full frames)
    uint8_t  format_bytes[64];     // difvif bytes of the last parsed full frame
    uint8_t  format_len;
    bool     truncated;            // ran out of entries/arena
};

void dv_clear(DvSet* s);

// Parse DV records. When format != nullptr the data is compact (values only)
// and the difvifs come from the format. base_offset is added to entry offsets.
void dv_parse(DvSet* s, const uint8_t* data, size_t len, uint16_t base_offset,
              const uint8_t* format = nullptr, size_t format_len = 0);

// Parse a synthetic entry from key hex (e.g. "037E") + value bytes; used by
// ixml extraction. Returns false when out of space.
bool dv_add_synthetic(DvSet* s, const char* dvk_hex, const uint8_t* value, size_t value_len, uint16_t offset);

const DvEntry* dv_find(const DvSet* s, const char* key);

// Value extraction (DVEntry::extract*)
bool dv_extract_double(const DvEntry* e, double* out, bool auto_scale, bool force_unsigned);
bool dv_extract_long(const DvEntry* e, uint64_t* out);
void dv_extract_hex(const DvEntry* e, char* out, size_t out_max);               // raw hex, wire order
void dv_extract_readable(const DvEntry* e, char* out, size_t out_max, bool reversed_mode);

struct DvDate {
    int year, month, day, hour, minute, second;
    bool has_time;
    bool has_seconds;
};
bool dv_extract_date(const DvEntry* e, DvDate* out);

// Seconds since the unix epoch for a (possibly unnormalized) civil date/time,
// normalized like mktime() does.
double civil_to_unix(int y, int mon, int d, int h, int mi, int s);
void   unix_to_civil(double t, int* y, int* mon, int* d, int* h, int* mi, int* s);

} // namespace wmb
