// wM-Buster ADV — wM-Bus type tables and unit conversion.
// GPL-3.0
#include "wmbus/types.h"

#include <math.h>
#include <string.h>

namespace wmb {

// ---------------------------------------------------------------------------
// Quantities / units
// ---------------------------------------------------------------------------
const char* quantity_name(Quantity q) {
    switch (q) {
#define X(n) case Quantity::n: return #n;
        WMB_LIST_OF_QUANTITIES
#undef X
    default: return "Unknown";
    }
}

Unit quantity_default_unit(Quantity q) {
    switch (q) {
    case Quantity::Time: return Unit::Hour;
    case Quantity::Length: return Unit::M;
    case Quantity::Mass: return Unit::KG;
    case Quantity::Amperage: return Unit::Ampere;
    case Quantity::Temperature: return Unit::C;
    case Quantity::AmountOfSubstance: return Unit::MOL;
    case Quantity::LuminousIntensity: return Unit::CD;
    case Quantity::Energy: return Unit::KWH;
    case Quantity::Reactive_Energy: return Unit::KVARH;
    case Quantity::Apparent_Energy: return Unit::KVAH;
    case Quantity::Power: return Unit::KW;
    case Quantity::Reactive_Power: return Unit::KVAR;
    case Quantity::Apparent_Power: return Unit::KVA;
    case Quantity::Volume: return Unit::M3;
    case Quantity::Flow: return Unit::M3H;
    case Quantity::Voltage: return Unit::Volt;
    case Quantity::Frequency: return Unit::HZ;
    case Quantity::Pressure: return Unit::BAR;
    case Quantity::PointInTime: return Unit::DateTimeLT;
    case Quantity::RelativeHumidity: return Unit::RH;
    case Quantity::HCA: return Unit::HCA;
    case Quantity::Text: return Unit::TXT;
    case Quantity::Angle: return Unit::DEGREE;
    case Quantity::Dimensionless: return Unit::COUNTER;
    default: return Unit::Unknown;
    }
}

const char* unit_suffix(Unit u) {
    switch (u) {
#define X(cname, lcname, hr, q) case Unit::cname: return #lcname;
        WMB_LIST_OF_UNITS
#undef X
    default: return "?";
    }
}

const char* unit_human(Unit u) {
    switch (u) {
#define X(cname, lcname, hr, q) case Unit::cname: return hr;
        WMB_LIST_OF_UNITS
#undef X
    default: return "";
    }
}

Quantity unit_quantity(Unit u) {
    switch (u) {
#define X(cname, lcname, hr, q) case Unit::cname: return Quantity::q;
        WMB_LIST_OF_UNITS
#undef X
    default: return Quantity::Unknown;
    }
}

// ---------------------------------------------------------------------------
// VIF ranges
// ---------------------------------------------------------------------------
const char* vif_range_name(VifRange r) {
    switch (r) {
    case VifRange::Any: return "Any";
#define X(name, from, to, q, u) case VifRange::name: return #name;
        WMB_LIST_OF_VIF_RANGES
#undef X
    default: return "None";
    }
}

Unit vif_range_default_unit(VifRange r) {
    switch (r) {
#define X(name, from, to, q, u) case VifRange::name: return Unit::u;
        WMB_LIST_OF_VIF_RANGES
#undef X
    default: return Unit::Unknown;
    }
}

Quantity vif_range_quantity(VifRange r) {
    switch (r) {
#define X(name, from, to, q, u) case VifRange::name: return Quantity::q;
        WMB_LIST_OF_VIF_RANGES
#undef X
    default: return Quantity::Unknown;
    }
}

VifRange vif_to_range(uint16_t vif) {
#define X(name, from, to, q, u) if ((from) <= vif && vif <= (to) && !((from) == 0 && (to) == 0 && VifRange::name >= VifRange::AnyVolumeVIF)) return VifRange::name;
    WMB_LIST_OF_VIF_RANGES
#undef X
    return VifRange::None;
}

struct RangeRow { VifRange r; uint16_t from, to; };
static const RangeRow RANGE_ROWS[] = {
#define X(name, from, to, q, u) { VifRange::name, from, to },
    WMB_LIST_OF_VIF_RANGES
#undef X
};

bool vif_in_range(uint16_t vif, VifRange r) {
    if (r == VifRange::AnyVolumeVIF) return vif_in_range(vif, VifRange::Volume);
    if (r == VifRange::AnyEnergyVIF)
        return vif_in_range(vif, VifRange::EnergyWh) || vif_in_range(vif, VifRange::EnergyMJ) ||
               vif_in_range(vif, VifRange::EnergyMWh) || vif_in_range(vif, VifRange::EnergyGJ);
    if (r == VifRange::AnyPowerVIF) return vif_in_range(vif, VifRange::PowerW) || vif_in_range(vif, VifRange::PowerJh);
    for (const RangeRow& row : RANGE_ROWS) {
        if (row.r == r) return row.from <= vif && vif <= row.to;
    }
    return r == VifRange::Any;
}

VifCombinable combinable_from_raw(uint16_t raw) {
#define X(name, from, to) if ((from) <= raw && raw <= (to)) return VifCombinable::name;
    WMB_LIST_OF_VIF_COMBINABLES
#undef X
    return VifCombinable::None;
}

const char* combinable_name(VifCombinable c) {
    switch (c) {
    case VifCombinable::Any: return "Any";
#define X(name, from, to) case VifCombinable::name: return #name;
        WMB_LIST_OF_VIF_COMBINABLES
#undef X
    default: return "None";
    }
}

// ---------------------------------------------------------------------------
// VIF scaling (value = raw / scale), identical to upstream vifScale().
// ---------------------------------------------------------------------------
double vif_scale(uint16_t vif) {
    vif &= 0x7f7f;
    if (vif <= 0x37) {
        // Energy Wh/J, volume, mass, on/operating time, power W and J/h.
        static const double E8[8] = { 1000000.0, 100000.0, 10000.0, 1000.0, 100.0, 10.0, 1.0, 0.1 };
        if (vif >= 0x18 && vif <= 0x1F) {
            static const double MASS[8] = { 1000.0, 100.0, 10.0, 1.0, 0.1, 0.01, 0.001, 0.0001 };
            return MASS[vif - 0x18];
        }
        if (vif >= 0x20 && vif <= 0x27) {
            static const double T[4] = { 3600.0, 60.0, 1.0, 1.0 / 24.0 };
            return T[vif & 3];
        }
        return E8[vif & 7];
    }
    if (vif >= 0x38 && vif <= 0x3F) {
        static const double F[8] = { 1000000.0, 100000.0, 10000.0, 1000.0, 100.0, 10.0, 1.0, 0.1 };
        return F[vif - 0x38];
    }
    if (vif >= 0x40 && vif <= 0x47) {
        static const double F[8] = { 600000000.0, 60000000.0, 6000000.0, 600000.0, 60000.0, 6000.0, 600.0, 60.0 };
        return F[vif - 0x40];
    }
    if (vif >= 0x48 && vif <= 0x4F) {
        static const double F[8] = { 1000000000.0 * 3600, 100000000.0 * 3600, 10000000.0 * 3600, 1000000.0 * 3600,
                                     100000.0 * 3600, 10000.0 * 3600, 1000.0 * 3600, 100.0 * 3600 };
        return F[vif - 0x48];
    }
    if (vif >= 0x50 && vif <= 0x57) {
        static const double MF[8] = { 1000.0, 100.0, 10.0, 1.0, 0.1, 0.01, 0.001, 0.0001 };
        return MF[vif - 0x50];
    }
    if (vif >= 0x58 && vif <= 0x6B) {
        static const double T4[4] = { 1000.0, 100.0, 10.0, 1.0 };
        return T4[vif & 3];
    }
    switch (vif) {
    case 0x6C: case 0x6D: case 0x6E: return 1.0;
    case 0x6F: return -1.0;
    case 0x70: case 0x74: return 3600.0;
    case 0x71: case 0x75: return 60.0;
    case 0x72: case 0x76: return 1.0;
    case 0x73: case 0x77: return 1.0 / 24.0;
    case 0x7C: return 1.0;
    case 0x7B00: case 0x7B01: case 0x7B08: case 0x7B09: {
        double e = (vif & 0x1) + 2;
        return pow(10.0, -e);
    }
    case 0x7B1A: return 10.0;
    case 0x7B1B: return 1.0;
    case 0x7D08: return 1.0;
    case 0x7D31: return 60.0;
    case 0x7D32: return 1.0;
    case 0x7D33: return 1.0 / 24.0;
    case 0x7D61: return 1.0;
    case 0x7D74: return 1.0;
    case 0x7D2C: return 3600.0;
    case 0x7D2D: return 60.0;
    case 0x7D2E: return 1.0;
    case 0x7D2F: return 1.0 / 24.0;
    case 0x7D3A: return 1.0;
    default: break;
    }
    if (vif >= 0x7D40 && vif <= 0x7D4F) { double e = (double)(vif & 0xf) - 9; return pow(10.0, -e); }
    if (vif >= 0x7D50 && vif <= 0x7D5F) { double e = (double)(vif & 0xf) - 12; return pow(10.0, -e); }
    return -1000000;
}

// ---------------------------------------------------------------------------
// Unit conversion. Linear units are expressed as a factor to a common base
// per quantity; temperatures and dBm are handled explicitly.
// ---------------------------------------------------------------------------
static bool linear_scale(Unit u, double* s) {
    switch (u) {
    case Unit::Second: *s = 1.0; return true;
    case Unit::Minute: *s = 60.0; return true;
    case Unit::Hour: *s = 3600.0; return true;
    case Unit::Day: *s = 86400.0; return true;
    case Unit::Year: *s = 3600.0 * 24.0 * 365.2425; return true;
    case Unit::Month: *s = 3600.0 * 24.0 * 365.2425 / 12.0; return true;
    case Unit::M: *s = 1.0; return true;
    case Unit::KG: *s = 1.0; return true;
    case Unit::Ampere: *s = 1.0; return true;
    case Unit::MOL: *s = 1.0; return true;
    case Unit::CD: *s = 1.0; return true;
    case Unit::WH: *s = 3.6e3; return true;
    case Unit::KWH: *s = 3.6e6; return true;
    case Unit::MJ: *s = 1.0e6; return true;
    case Unit::GJ: *s = 1.0e9; return true;
    case Unit::GCAL: *s = 1162.22 * 3.6e6; return true;
    case Unit::KVARH: *s = 3.6e6; return true;
    case Unit::KVAH: *s = 3.6e6; return true;
    case Unit::M3C: *s = 1.0; return true;
    case Unit::W: *s = 1.0; return true;
    case Unit::KW: *s = 1000.0; return true;
    case Unit::JH: *s = 1.0 / 3600.0; return true;
    case Unit::MJH: *s = 1000000.0 / 3600.0; return true;
    case Unit::KVAR: *s = 1000.0; return true;
    case Unit::KVA: *s = 1000.0; return true;
    case Unit::M3CH: *s = 1.0; return true;
    case Unit::M3: *s = 1.0; return true;
    case Unit::L: *s = 1.0 / 1000.0; return true;
    case Unit::M3H: *s = 1.0; return true;
    case Unit::LH: *s = 1.0 / 1000.0; return true;
    case Unit::Volt: *s = 1.0; return true;
    case Unit::HZ: *s = 1.0; return true;
    case Unit::PA: *s = 1.0; return true;
    case Unit::BAR: *s = 100000.0; return true;
    case Unit::UnixTimestamp:
    case Unit::DateTimeUTC:
    case Unit::DateTimeLT:
    case Unit::DateLT:
    case Unit::TimeLT: *s = 1.0; return true;
    case Unit::RH: *s = 1.0; return true;
    case Unit::HCA: *s = 1.0; return true;
    case Unit::TXT: *s = 1.0; return true;
    case Unit::DEGREE: *s = 1.0; return true;
    case Unit::RADIAN: *s = 180.0 / M_PI; return true;
    case Unit::COUNTER: case Unit::FACTOR: case Unit::NUMBER: *s = 1.0; return true;
    case Unit::PERCENTAGE: *s = 0.01; return true;
    case Unit::PPM: *s = 0.000001; return true;
    default: return false;
    }
}

static bool is_temp(Unit u) { return u == Unit::C || u == Unit::K || u == Unit::F; }

bool units_convertible(Unit from, Unit to) {
    if (from == to) return true;
    if (from == Unit::Unknown || to == Unit::Unknown) return false;
    if (is_temp(from) && is_temp(to)) return true;
    if (from == Unit::DBM || to == Unit::DBM) {
        return (from == Unit::W || to == Unit::W || from == Unit::KW || to == Unit::KW);
    }
    Quantity qf = unit_quantity(from), qt = unit_quantity(to);
    if (qf != qt) return false;
    double a, b;
    return linear_scale(from, &a) && linear_scale(to, &b);
}

double unit_convert(double v, Unit from, Unit to) {
    if (from == to || from == Unit::Unknown || to == Unit::Unknown) return v;
    if (is_temp(from) && is_temp(to)) {
        double c = v;
        if (from == Unit::K) c = v - 273.15;
        else if (from == Unit::F) c = (v - 32.0) * 5.0 / 9.0;
        if (to == Unit::K) return c + 273.15;
        if (to == Unit::F) return c * 9.0 / 5.0 + 32.0;
        return c;
    }
    if (from == Unit::DBM) {
        double w = pow(10.0, v / 10.0) / 1000.0;
        return unit_convert(w, Unit::W, to);
    }
    if (to == Unit::DBM) {
        double w = unit_convert(v, from, Unit::W);
        return 10.0 * log10(w * 1000.0);
    }
    double a, b;
    if (!linear_scale(from, &a) || !linear_scale(to, &b)) return v;
    if (unit_quantity(from) != unit_quantity(to)) return v;
    return v * a / b;
}

bool unit_override_conversion(Unit from, Unit to) {
    return from == Unit::KWH && (to == Unit::KVARH || to == Unit::KVAH);
}

// ---------------------------------------------------------------------------
// Manufacturer / media
// ---------------------------------------------------------------------------
void mfct_to_str(uint16_t m, char out[4]) {
    out[0] = (char)(((m >> 10) & 0x1F) + 64);
    out[1] = (char)(((m >> 5) & 0x1F) + 64);
    out[2] = (char)((m & 0x1F) + 64);
    out[3] = '\0';
}

uint16_t mfct_from_str(const char* s) {
    return (uint16_t)((((s[0] - 64) & 0x1F) << 10) | (((s[1] - 64) & 0x1F) << 5) | ((s[2] - 64) & 0x1F));
}

const char* media_name(uint8_t t, uint16_t mfct) {
    switch (t) {
    case 0x00: return "other";
    case 0x01: return "oil";
    case 0x02: return "electricity";
    case 0x03: return "gas";
    case 0x04: return "heat";
    case 0x05: return "steam";
    case 0x06: return "warm water";
    case 0x07: return "water";
    case 0x08: return "heat cost allocation";
    case 0x09: return "compressed air";
    case 0x0a: return "cooling load volume at outlet";
    case 0x0b: return "cooling load volume at inlet";
    case 0x0c: return "heat volume at inlet";
    case 0x0d: return "heat/cooling load";
    case 0x0e: return "bus/system component";
    case 0x0f: return "unknown";
    case 0x15: return "hot water";
    case 0x16: return "cold water";
    case 0x17: return "hot/cold water";
    case 0x18: return "pressure";
    case 0x19: return "a/d converter";
    case 0x1A: return "smoke detector";
    case 0x1B: return "room sensor";
    case 0x1C: return "gas detector";
    case 0x20: return "breaker";
    case 0x21: return "valve";
    case 0x25: return "customer unit (display device)";
    case 0x28: return "waste water";
    case 0x29: return "garbage";
    case 0x36: return "radio converter (system side)";
    case 0x37: return "radio converter (meter side)";
    default: break;
    }
    if (t <= 0x3F) return "reserved";
    if ((mfct & 0x7fff) == 0x5068) { // TCH
        switch (t) {
        case 0x62: return "warm water";
        case 0x72: return "cold water";
        case 0x80: return "heat cost allocator";
        case 0xC3: return "heat";
        case 0x43: return "heat";
        case 0xf0: return "smoke detector";
        default: break;
        }
    }
    return "Unknown";
}

} // namespace wmb
