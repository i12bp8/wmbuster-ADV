// wM-Buster ADV — the decoding pipeline: frame -> telegram (decrypted) ->
// DV records -> driver fields (or a generic OMS decode when no driver
// matches). Port of the wmbusmeters field extraction semantics.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/dv.h"
#include "wmbus/driver_table.h"
#include "wmbus/frame.h"
#include "wmbus/ixml.h"
#include "wmbus/telegram.h"

namespace wmb {

#ifndef WMB_MAX_FIELDS
#define WMB_MAX_FIELDS 72
#endif

struct OutField {
    char     name[48];      // json key incl. unit suffix ("total_m3"); text fields have no suffix
    char     vname[40];     // name without unit, templates expanded
    Quantity quantity;
    Unit     unit;          // display unit (TXT for text)
    double   value;         // numeric value (NAN = null); PointInTime: unix seconds
    const char* text;       // text value (points into DecodeResult::pool, never null)
    bool     is_text;
    bool     hidden;
    bool     is_status;
    int16_t  field_index;   // driver field index (ordering), -1 for generic
    uint16_t offset;        // telegram offset of the source record (ordering)
};

enum class DecodeStatus : uint8_t {
    Ok = 0,            // fields decoded (driver or generic)
    Encrypted,         // encrypted and no (working) key
    CompactUnknown,    // compact frame whose format has not been seen yet
    MfctPayload,       // manufacturer specific payload without a driver
    NoPayload,         // header only / empty payload
    BadHeader,         // unsupported CI / parse failure
};
const char* decode_status_name(DecodeStatus s);

struct DecodeResult {
    DecodeStatus     status;
    const DriverDef* driver;       // driver used (nullptr for generic)
    char             id[9];
    char             mfct[4];
    uint16_t         mfct_code;
    uint8_t          version;
    uint8_t          type;
    const char*      media;
    LinkMode         mode;
    DecryptStatus    decrypt;
    uint8_t          ci;
    uint8_t          tpl_sts;
    uint8_t          sec_mode;
    OutField         fields[WMB_MAX_FIELDS];
    int              num_fields;
    char             pool[4096];   // storage for OutField::text (do not memcpy results)
    uint16_t         pool_used;
};

struct DecodeOptions {
    const uint8_t*   key;             // meter key or nullptr
    uint8_t          key_len;         // 16 (AES, also when 0) or 8 (DES)
    const DriverDef* forced_driver;   // nullptr = auto detect by M/V/T
    bool             generic_fallback;// decode OMS records generically when no driver
    uint32_t         now_unix;        // current time for DES mode 3, 0 = unknown
    bool             simulated;       // replayed/pasted telegram (may be decrypted already)
    bool             decode_bad_tag;  // AES-CCM: decode content whose tag fails, flagged FAILED_DECODE,
                                      // like wmbusmeters. Off: reported as a wrong key instead.
};

// All working memory for one decode. Keep one instance (static / heap) and
// guard it with a mutex when shared between tasks.
struct Decoder {
    Telegram     t;
    DvSet        dv;
    IxmlResult   ix;
    DecodeResult res;
    char         hex[2 * WMB_FRAME_MAX + 2];
};

// Decode a frame. Always fills res.id/mfct/type etc. Returns true when
// fields were produced (status Ok).
bool wmbus_decode(Decoder* d, const Frame& f, const DecodeOptions& opt);

// Compact frame format cache (formats learned from full frames).
void compact_cache_remember(uint16_t mfct, uint8_t ver, uint8_t type, uint16_t sig, const uint8_t* fmt, uint8_t len);
bool compact_cache_lookup(uint16_t mfct, uint8_t ver, uint8_t type, uint16_t sig, const uint8_t** fmt, uint8_t* len);
void compact_cache_clear();

// Helpers for consumers.
const OutField* result_find(const DecodeResult* r, const char* name);
// Format a field value for display ("123.456 m3", "2024-05-01", "OK").
void field_format(const OutField* f, char* out, size_t out_max, bool with_unit = true);
// Render the result as a wmbusmeters compatible JSON object.
size_t result_to_json(const DecodeResult* r, char* out, size_t out_max, const char* name, const char* timestamp);

// Engine internals ----------------------------------------------------------
OutField* result_add_numeric(DecodeResult* r, const char* vname, Quantity q, Unit u, double v, int field_index, uint16_t offset);
OutField* result_add_text(DecodeResult* r, const char* vname, const char* text, int field_index, uint16_t offset);
void      engine_run_driver(Decoder* d, const DriverDef* drv, const uint8_t* key);
void      engine_generic(Decoder* d);

} // namespace wmb
