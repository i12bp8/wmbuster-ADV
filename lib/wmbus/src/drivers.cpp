// wM-Buster ADV — driver lookup (M/V/T detection like wmbusmeters, by name).
// GPL-3.0
#include "wmbus/driver_table.h"

#include <string.h>

namespace wmb {

const char* meter_type_name(MeterType t) {
    switch (t) {
    case MeterType::AutoMeter: return "AutoMeter";
    case MeterType::UnknownMeter: return "UnknownMeter";
    case MeterType::DoorWindowDetector: return "DoorWindowDetector";
    case MeterType::ElectricityMeter: return "ElectricityMeter";
    case MeterType::GasMeter: return "GasMeter";
    case MeterType::HeatCoolingMeter: return "HeatCoolingMeter";
    case MeterType::HeatCostAllocationMeter: return "HeatCostAllocationMeter";
    case MeterType::HeatMeter: return "HeatMeter";
    case MeterType::PressureSensor: return "PressureSensor";
    case MeterType::PulseCounter: return "PulseCounter";
    case MeterType::Repeater: return "Repeater";
    case MeterType::SmokeDetector: return "SmokeDetector";
    case MeterType::TempHygroMeter: return "TempHygroMeter";
    case MeterType::WaterMeter: return "WaterMeter";
    }
    return "UnknownMeter";
}

const DriverDef* driver_detect(uint16_t mfct, uint8_t version, uint8_t type) {
    for (size_t i = 0; i < DRIVERS_LEN; ++i) {
        const DriverDef* d = &DRIVERS[i];
        for (uint8_t j = 0; j < d->num_detects; ++j) {
            const DriverDetect& det = d->detects[j];
            if (det.mfct == 0 && det.version == 0 && det.type == 0) continue;
            bool vm = det.version == 0xFF || det.version == version;
            bool tm = det.type == 0xFF || det.type == type;
            if ((det.mfct & 0x7fff) == (mfct & 0x7fff) && vm && tm) return d;
        }
    }
    return nullptr;
}

static bool in_list(const char* list, const char* name) {
    if (!list) return false;
    size_t n = strlen(name);
    const char* p = list;
    while (*p) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

const DriverDef* driver_by_name(const char* name) {
    if (!name || !name[0]) return nullptr;
    for (size_t i = 0; i < DRIVERS_LEN; ++i) if (strcmp(DRIVERS[i].name, name) == 0) return &DRIVERS[i];
    for (size_t i = 0; i < DRIVERS_LEN; ++i) if (in_list(DRIVERS[i].aliases, name)) return &DRIVERS[i];
    return nullptr;
}

} // namespace wmb
