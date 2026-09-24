// wM-Buster ADV — decode pipeline, compact frame cache, generic OMS decode
// and result formatting (display + wmbusmeters style JSON).
// GPL-3.0
#include "wmbus/engine.h"
#include "wmbus/lookup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace wmb {

// ---------------------------------------------------------------------------
// Compact frame format cache (formats learned from full frames, keyed by the
// crc16 signature of the difvif bytes like upstream's hash_to_format_).
// ---------------------------------------------------------------------------
#define COMPACT_SLOTS 24
struct CompactSlot {
    uint16_t sig;
    uint8_t len;
    uint32_t age;
    uint8_t fmt[64];
};
static CompactSlot s_compact[COMPACT_SLOTS];
static uint32_t s_compact_clock = 0;

void compact_cache_remember(uint16_t, uint8_t, uint8_t, uint16_t sig, const uint8_t* fmt, uint8_t len) {
    if (len == 0 || len > sizeof(s_compact[0].fmt)) return;
    CompactSlot* victim = &s_compact[0];
    for (int i = 0; i < COMPACT_SLOTS; ++i) {
        CompactSlot* s = &s_compact[i];
        if (s->len && s->sig == sig) { s->age = ++s_compact_clock; return; }
        if (s->age < victim->age) victim = s;
    }
    victim->sig = sig;
    victim->len = len;
    victim->age = ++s_compact_clock;
    memcpy(victim->fmt, fmt, len);
}

bool compact_cache_lookup(uint16_t, uint8_t, uint8_t, uint16_t sig, const uint8_t** fmt, uint8_t* len) {
    for (int i = 0; i < COMPACT_SLOTS; ++i) {
        if (s_compact[i].len && s_compact[i].sig == sig) {
            *fmt = s_compact[i].fmt;
            *len = s_compact[i].len;
            s_compact[i].age = ++s_compact_clock;
            return true;
        }
    }
    return false;
}

void compact_cache_clear() { memset(s_compact, 0, sizeof(s_compact)); }

// ---------------------------------------------------------------------------
// Generic decode: every standard DV record becomes a field.
// ---------------------------------------------------------------------------
static const char* generic_base(VifRange r) {
    switch (r) {
    case VifRange::Volume: return "volume";
    case VifRange::OnTime: return "on_time";
    case VifRange::OperatingTime: return "operating_time";
    case VifRange::VolumeFlow: return "volume_flow";
    case VifRange::FlowTemperature: return "flow_temperature";
    case VifRange::ReturnTemperature: return "return_temperature";
    case VifRange::TemperatureDifference: return "temperature_difference";
    case VifRange::ExternalTemperature: return "external_temperature";
    case VifRange::Pressure: return "pressure";
    case VifRange::HeatCostAllocation: return "consumption";
    case VifRange::Date: return "date";
    case VifRange::DateTime: return "datetime";
    case VifRange::EnergyMJ: case VifRange::EnergyWh: case VifRange::EnergyMWh: case VifRange::EnergyGJ: return "energy";
    case VifRange::PowerW: case VifRange::PowerJh: return "power";
    case VifRange::ActualityDuration: return "actuality_duration";
    case VifRange::FabricationNo: return "fabrication_no";
    case VifRange::EnhancedIdentification: return "enhanced_id";
    case VifRange::RelativeHumidity: return "humidity";
    case VifRange::AccessNumber: return "access_number";
    case VifRange::Medium: return "medium";
    case VifRange::Manufacturer: return "manufacturer";
    case VifRange::ParameterSet: return "parameter_set";
    case VifRange::ModelVersion: return "model_version";
    case VifRange::HardwareVersion: return "hardware_version";
    case VifRange::FirmwareVersion: return "firmware_version";
    case VifRange::SoftwareVersion: return "software_version";
    case VifRange::Location: return "location";
    case VifRange::Customer: return "customer";
    case VifRange::ErrorFlags: return "error_flags";
    case VifRange::DigitalOutput: return "digital_output";
    case VifRange::DigitalInput: return "digital_input";
    case VifRange::DurationSinceReadout: return "since_readout";
    case VifRange::DurationOfTariff: return "tariff_duration";
    case VifRange::Dimensionless: return "dimensionless";
    case VifRange::Voltage: return "voltage";
    case VifRange::Amperage: return "current";
    case VifRange::ResetCounter: return "reset_counter";
    case VifRange::CumulationCounter: return "cumulation_counter";
    case VifRange::SpecialSupplierInformation: return "supplier_info";
    case VifRange::RemainingBattery: return "battery_remaining";
    default: return nullptr;
    }
}

void engine_generic(Decoder* d) {
    DecodeResult* r = &d->res;
    for (int i = 0; i < d->dv.n; ++i) {
        const DvEntry* e = &d->dv.e[i];
        char name[48];
        size_t o = 0;
        VifRange vr = vif_to_range(e->vif & 0x7f7f);
        const char* base = generic_base(vr);
        if (e->dif == 0x0F) {
            char hex[80];
            dv_extract_hex(e, hex, sizeof(hex));
            result_add_text(r, "mfct_data", hex, -1, e->offset);
            continue;
        }
        if (e->mtype == MeasurementType::Maximum) o += (size_t)snprintf(name + o, sizeof(name) - o, "max_");
        else if (e->mtype == MeasurementType::Minimum) o += (size_t)snprintf(name + o, sizeof(name) - o, "min_");
        else if (e->mtype == MeasurementType::AtError) o += (size_t)snprintf(name + o, sizeof(name) - o, "error_");
        if (base) o += (size_t)snprintf(name + o, sizeof(name) - o, "%s", base);
        else o += (size_t)snprintf(name + o, sizeof(name) - o, "vif_%X", e->vif);
        for (int c = 0; c < e->ncomb && o < sizeof(name); ++c) {
            VifCombinable vc = combinable_from_raw(e->comb_raw[c]);
            if (vc == VifCombinable::ForwardFlow) o += (size_t)snprintf(name + o, sizeof(name) - o, "_forward");
            else if (vc == VifCombinable::BackwardFlow) o += (size_t)snprintf(name + o, sizeof(name) - o, "_backward");
            else if (vc == VifCombinable::AtPhase1) o += (size_t)snprintf(name + o, sizeof(name) - o, "_l1");
            else if (vc == VifCombinable::AtPhase2) o += (size_t)snprintf(name + o, sizeof(name) - o, "_l2");
            else if (vc == VifCombinable::AtPhase3) o += (size_t)snprintf(name + o, sizeof(name) - o, "_l3");
            else o += (size_t)snprintf(name + o, sizeof(name) - o, "_x%X", e->comb_raw[c]);
        }
        if (e->storage && o < sizeof(name)) o += (size_t)snprintf(name + o, sizeof(name) - o, "_s%u", (unsigned)e->storage);
        if (e->tariff && o < sizeof(name)) o += (size_t)snprintf(name + o, sizeof(name) - o, "_t%u", (unsigned)e->tariff);
        if (e->subunit && o < sizeof(name)) o += (size_t)snprintf(name + o, sizeof(name) - o, "_u%u", (unsigned)e->subunit);

        Quantity q = vif_range_quantity(vr);
        if (vr == VifRange::Date || vr == VifRange::DateTime) {
            DvDate dd;
            double t = NAN;
            if (dv_extract_date(e, &dd) && dd.month >= 1 && dd.day >= 1) t = civil_to_unix(dd.year, dd.month, dd.day, dd.hour, dd.minute, dd.second);
            result_add_numeric(r, name, Quantity::PointInTime, vr == VifRange::Date ? Unit::DateLT : Unit::DateTimeLT, t, -1, e->offset);
            continue;
        }
        if (q == Quantity::Text || vr == VifRange::None) {
            char text[80];
            if (vr == VifRange::ErrorFlags || vr == VifRange::None) dv_extract_hex(e, text, sizeof(text));
            else dv_extract_readable(e, text, sizeof(text), false);
            if (vr == VifRange::None) {
                // Unknown VIF: try a plain number when the record is numeric.
                double v;
                if (dv_extract_double(e, &v, false, false)) {
                    result_add_numeric(r, name, Quantity::Dimensionless, Unit::COUNTER, v, -1, e->offset);
                    continue;
                }
            }
            result_add_text(r, name, text, -1, e->offset);
            continue;
        }
        double v;
        if (!dv_extract_double(e, &v, true, false)) {
            if ((e->dif & 0x0F) == 0x0D) {
                char text[80];
                dv_extract_readable(e, text, sizeof(text), false);
                result_add_text(r, name, text, -1, e->offset);
            }
            continue;
        }
        Unit u = vif_range_default_unit(vr);
        if (u == Unit::Unknown) u = quantity_default_unit(q);
        result_add_numeric(r, name, q, u, v, -1, e->offset);
    }
    // TPL status
    if (d->t.tpl_sts_offset >= 0) {
        char s[96];
        tpl_status_standard(d->t.tpl_sts, s, sizeof(s));
        if (d->t.tpl_sts & 0xe0) {
            size_t l = strlen(s);
            if (!strcmp(s, "OK")) l = 0;
            snprintf(s + l, sizeof(s) - l, "%sMFCT_%02X", l ? " " : "", d->t.tpl_sts & 0xe0);
        }
        OutField* f = result_add_text(r, "status", s, -2, d->t.tpl_sts_offset);
        if (f) f->is_status = true;
    }
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
bool wmbus_decode(Decoder* d, const Frame& f, const DecodeOptions& opt) {
    DecodeResult* r = &d->res;
    r->status = DecodeStatus::BadHeader;
    r->driver = nullptr;
    r->num_fields = 0;
    r->pool_used = 0;
    r->id[0] = 0;
    r->mfct[0] = 0;
    r->media = "unknown";
    r->mode = f.mode;
    r->decrypt = DecryptStatus::NotEncrypted;
    r->ci = 0;
    r->tpl_sts = 0;
    r->sec_mode = 0;

    TelegramOptions to;
    memset(&to, 0, sizeof(to));
    to.now_unix = opt.now_unix;
    to.simulated = opt.simulated;
    to.decode_bad_tag = opt.decode_bad_tag;
    const uint8_t* aes_key = opt.key && (opt.key_len == 0 || opt.key_len == 16) ? opt.key : nullptr;
    Telegram* t = &d->t;
    if (!telegram_parse(f, to, t)) return false;

    const DriverDef* drv = opt.forced_driver;
    if (!drv) drv = driver_detect(t->mvt_mfct(), t->mvt_version(), t->mvt_type());

    bool reparse = t->decrypt != DecryptStatus::NotEncrypted || (drv && (drv->num_default_keys || (drv->flags & DRV_BUGGY_SANXING_609B)));
    if (reparse && (opt.key || (drv && drv->num_default_keys) || (drv && (drv->flags & DRV_BUGGY_SANXING_609B)))) {
        to.key = opt.key;
        to.key_len = opt.key_len;
        if (drv) {
            to.default_keys = drv->default_keys;
            to.num_default_keys = drv->num_default_keys;
            to.permit_sanxing_609b = (drv->flags & DRV_BUGGY_SANXING_609B) != 0;
        }
        telegram_parse(f, to, t);
    }

    t->meter_id(r->id);
    r->mfct_code = t->mvt_mfct();
    mfct_to_str(r->mfct_code, r->mfct);
    r->version = t->mvt_version();
    r->type = t->mvt_type();
    r->media = (drv && drv->force_media) ? drv->force_media : media_name(r->type, r->mfct_code);
    r->mode = f.mode;
    r->decrypt = t->decrypt;
    r->ci = t->tpl_ci;
    r->tpl_sts = t->tpl_sts;
    r->sec_mode = t->tpl_sec_mode;
    r->driver = drv;

    if (t->decrypt == DecryptStatus::NoKey || t->decrypt == DecryptStatus::WrongKey ||
        t->decrypt == DecryptStatus::Unsupported) {
        r->status = DecodeStatus::Encrypted;
        return false;
    }
    if (!t->header_ok) {
        r->status = DecodeStatus::BadHeader;
        return false;
    }

    dv_clear(&d->dv);
    const uint8_t* pl = t->payload();
    size_t pln = t->payload_len();
    if (t->compact) {
        const uint8_t* fmt = nullptr;
        uint8_t fmt_len = 0;
        bool found = false;
        if (drv) {
            for (int i = 0; i < drv->num_compact_formats; ++i) {
                if (drv->compact_formats[i].signature == t->format_signature) {
                    fmt = drv->compact_formats[i].difvif;
                    fmt_len = drv->compact_formats[i].len;
                    found = true;
                    break;
                }
            }
        }
        if (!found) found = compact_cache_lookup(r->mfct_code, r->version, r->type, t->format_signature, &fmt, &fmt_len);
        if (!found) {
            r->status = DecodeStatus::CompactUnknown;
            return false;
        }
        dv_parse(&d->dv, pl, pln, t->header_size, fmt, fmt_len);
    } else if (!t->mfct_specific) {
        dv_parse(&d->dv, pl, pln, t->header_size);
        if (d->dv.format_len) {
            compact_cache_remember(r->mfct_code, r->version, r->type, d->dv.format_hash, d->dv.format_bytes, d->dv.format_len);
        }
    }

    if (drv) {
        engine_run_driver(d, drv, aes_key);
        r->status = DecodeStatus::Ok;
        return true;
    }
    if (!opt.generic_fallback) {
        r->status = t->mfct_specific ? DecodeStatus::MfctPayload : DecodeStatus::NoPayload;
        return false;
    }
    if (t->mfct_specific) {
        r->status = DecodeStatus::MfctPayload;
        return false;
    }
    engine_generic(d);
    r->status = r->num_fields > 0 ? DecodeStatus::Ok : DecodeStatus::NoPayload;
    return r->status == DecodeStatus::Ok;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
static void trim_number(char* s) {
    if (!strchr(s, '.')) return;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '0') s[--n] = 0;
    if (n > 0 && s[n - 1] == '.') s[--n] = 0;
    if (n == 0 || !strcmp(s, "-0")) strcpy(s, "0");
}

void field_format(const OutField* f, char* out, size_t out_max, bool with_unit) {
    if (f->is_text) {
        snprintf(out, out_max, "%s", f->text[0] ? f->text : "-");
        return;
    }
    if (isnan(f->value)) {
        snprintf(out, out_max, "-");
        return;
    }
    if (f->quantity == Quantity::PointInTime || unit_quantity(f->unit) == Quantity::PointInTime) {
        int y, mo, d, h, mi, s;
        unix_to_civil(f->value, &y, &mo, &d, &h, &mi, &s);
        if (f->unit == Unit::DateLT) snprintf(out, out_max, "%04d-%02d-%02d", y, mo, d);
        else if (f->unit == Unit::UnixTimestamp) snprintf(out, out_max, "%.0f", f->value);
        else snprintf(out, out_max, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
        return;
    }
    char num[40];
    double v = f->value;
    double a = fabs(v);
    if (a >= 100000) snprintf(num, sizeof(num), "%.1f", v);
    else if (a >= 1000) snprintf(num, sizeof(num), "%.2f", v);
    else snprintf(num, sizeof(num), "%.3f", v);
    trim_number(num);
    const char* u = with_unit ? unit_human(f->unit) : "";
    if (u && u[0]) snprintf(out, out_max, "%s %s", num, u);
    else snprintf(out, out_max, "%s", num);
}

static size_t json_str(char* out, size_t cap, size_t o, const char* s) {
    if (o < cap) out[o++] = '"';
    for (const char* p = s; *p && o + 2 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    if (o < cap) out[o++] = '"';
    return o;
}

static void json_number(double v, char* out, size_t cap) {
    if (isnan(v)) { snprintf(out, cap, "null"); return; }
    snprintf(out, cap, "%f", v);
    trim_number(out);
}

size_t result_to_json(const DecodeResult* r, char* out, size_t cap, const char* name, const char* timestamp) {
    size_t o = 0;
#define PUT(...) do { if (o < cap) { int w_ = snprintf(out + o, cap - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; } if (o >= cap) o = cap - 1; } while (0)
    PUT("{\"_\":\"telegram\",\"media\":");
    o = json_str(out, cap, o, r->media ? r->media : "unknown");
    PUT(",\"driver\":");
    o = json_str(out, cap, o, r->driver ? r->driver->name : "unknown");
    if (name) { PUT(",\"name\":"); o = json_str(out, cap, o, name); }
    PUT(",\"id\":");
    o = json_str(out, cap, o, r->id);
    for (int i = 0; i < r->num_fields; ++i) {
        const OutField* f = &r->fields[i];
        if (f->hidden) continue;
        PUT(",");
        o = json_str(out, cap, o, f->name);
        PUT(":");
        if (f->is_text) {
            if (!strcmp(f->text, "null")) PUT("null");
            else o = json_str(out, cap, o, f->text);
        } else if (unit_quantity(f->unit) == Quantity::PointInTime && f->unit != Unit::UnixTimestamp) {
            if (isnan(f->value)) PUT("null");
            else {
                char buf[32];
                field_format(f, buf, sizeof(buf), false);
                o = json_str(out, cap, o, buf);
            }
        } else {
            char num[40];
            json_number(f->value, num, sizeof(num));
            PUT("%s", num);
        }
    }
    if (timestamp) { PUT(",\"timestamp\":"); o = json_str(out, cap, o, timestamp); }
    PUT("}");
#undef PUT
    if (o < cap) out[o] = 0;
    return o;
}

} // namespace wmb
