// wM-Buster ADV — driver rule tables. Generated from the upstream wmbusmeters
// XMQ driver sources by tools/generate_drivers.py into drivers_generated.cpp.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/types.h"

namespace wmb {

// ---------------------------------------------------------------------------
// Lookups (translatebits)
// ---------------------------------------------------------------------------
enum class MapType : uint8_t { BitToString = 0, IndexToString = 1, DecimalsToString = 2 };
enum class TestBit : uint8_t { Set = 0, NotSet = 1 };

struct LookupMap {
    uint64_t    value;
    const char* name;
    TestBit     test;
};

struct LookupRule {
    const char*      name;
    MapType          type;
    uint64_t         mask;         // 0 = auto mask (all mapped bits)
    const char*      default_message;
    const LookupMap* maps;
    uint8_t          num_maps;
};

// ---------------------------------------------------------------------------
// ixml grammar programs (see ixml.h)
// ---------------------------------------------------------------------------
struct IxmlNode {
    uint8_t  type;     // IxmlNodeType
    uint8_t  flags;    // IXF_*
    uint16_t a;        // child start / literal index / rule index
    uint16_t b;        // child count / literal length / min
    uint16_t c;        // max (0xFFFF = unbounded)
};

struct IxmlRule {
    const char* name;
    uint16_t    body;        // node index
    const char* dvk;         // non-null when this element carries a @dvk attribute
};

struct IxmlGrammar {
    const IxmlNode* nodes;
    const uint16_t* children;
    const IxmlRule* rules;
    const char*     literals;    // concatenated literal text
    uint16_t        num_nodes;
    uint16_t        num_rules;
    uint16_t        start_rule;
};

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------
#define FATTR_HIDE               0x0001
#define FATTR_STATUS             0x0002
#define FATTR_INCLUDE_TPL_STATUS 0x0004
#define FATTR_DEPRECATED         0x0008
#define FATTR_INJECT_INTO_STATUS 0x0010
#define FATTR_REQUIRED           0x0020
#define FATTR_OPTIONAL           0x0040

// FieldMatch flags
#define FM_ACTIVE          0x0001
#define FM_DIFVIFKEY       0x0002
#define FM_MTYPE           0x0004
#define FM_VIF_RANGE       0x0008
#define FM_VIF_RAW         0x0010
#define FM_STORAGE         0x0020
#define FM_TARIFF          0x0040
#define FM_SUBUNIT         0x0080
#define FM_COMB_ANY        0x0100

struct FieldMatch {
    uint16_t        flags;
    const char*     difvifkey;
    MeasurementType mtype;
    VifRange        vif_range;
    uint16_t        vif_raw;
    uint32_t        storage_from, storage_to;
    uint16_t        tariff_from, tariff_to;
    uint16_t        subunit_from, subunit_to;
    uint8_t         index_nr;       // default 1
    uint8_t         num_combs;
    uint8_t         num_combs_raw;
    const uint8_t*  combs;          // VifCombinable values
    const uint16_t* combs_raw;
};

// FieldDef flags
#define FD_MATCH_ENTIRE_PAYLOAD 0x01
#define FD_MATCH_ENTIRE_FRAME   0x02
#define FD_HAS_NULL_VALUE       0x04
#define FD_TPL_AES_TRANSFORM    0x08
#define FD_READABLE_NORMAL      0x10
#define FD_READABLE_REVERSED    0x20

enum class VifScaling : uint8_t { Auto = 0, None = 1 };
enum class DifSignedness : uint8_t { Default = 0, Signed = 1, Unsigned = 2 };

struct FieldDef {
    const char*        name;          // may contain {storage_counter - 1 counter} templates
    Quantity           quantity;
    Unit               display_unit;
    Unit               force_unit;    // Unknown when unused
    uint16_t           attrs;
    uint8_t            flags;
    VifScaling         vif_scaling;
    DifSignedness      signedness;
    double             force_scale;   // 1.0 default
    double             null_value;
    FieldMatch         match;
    const char*        calculate;     // formula or nullptr
    const LookupRule*  lookups;
    uint8_t            num_lookups;
    const IxmlGrammar* ixml;
    int16_t            payload_offset, payload_length, tpl_acc_offset; // tpl_aes_cbc_iv transform
};

// Driver level flags
#define DRV_DIEHL_PRIOS         0x01
#define DRV_TRY_QUNDIS_DECODE   0x02
#define DRV_BUGGY_SANXING_609B  0x04

struct DriverDetect {
    uint16_t mfct;     // manufacturer code (masked with 0x7fff when compared)
    uint8_t  version;  // 0xFF = any
    uint8_t  type;     // 0xFF = any
};

struct CompactFormat {
    const uint8_t* difvif;
    uint8_t        len;
    uint16_t       signature;   // crc16 of difvif
};

enum class MeterType : uint8_t {
    AutoMeter, UnknownMeter, DoorWindowDetector, ElectricityMeter, GasMeter, HeatCoolingMeter,
    HeatCostAllocationMeter, HeatMeter, PressureSensor, PulseCounter, Repeater, SmokeDetector,
    TempHygroMeter, WaterMeter,
};
const char* meter_type_name(MeterType t);

struct DriverDef {
    const char*          name;
    const char*          aliases;        // comma separated or nullptr
    MeterType            meter_type;
    const char*          default_fields;
    const char*          force_media;    // nullptr unless the driver forces a media name
    uint8_t              flags;
    const DriverDetect*  detects;
    uint8_t              num_detects;
    const FieldDef*      fields;
    uint8_t              num_fields;
    const LookupRule*    tpl_status;     // manufacturer specific TPL status bits, or nullptr
    uint8_t              num_tpl_status;
    const CompactFormat* compact_formats;
    uint8_t              num_compact_formats;
    const uint8_t      (*default_keys)[16];
    uint8_t              num_default_keys;
};

extern const DriverDef DRIVERS[];
extern const size_t DRIVERS_LEN;

// Detection by manufacturer/version/type (exact, with wildcards).
const DriverDef* driver_detect(uint16_t mfct, uint8_t version, uint8_t type);
// Lookup by name or alias.
const DriverDef* driver_by_name(const char* name);

// Manufacturer full names (generated): returns nullptr when unknown.
const char* manufacturer_name(uint16_t mfct);

} // namespace wmb
