// wM-Buster ADV — decoding pipeline and driver field engine.
// GPL-3.0
#include "wmbus/engine.h"
#include "wmbus/aes.h"
#include "wmbus/formula.h"
#include "wmbus/lookup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace wmb {

const char* decode_status_name(DecodeStatus s) {
    switch (s) {
    case DecodeStatus::Ok: return "ok";
    case DecodeStatus::Encrypted: return "encrypted";
    case DecodeStatus::CompactUnknown: return "compact (format unknown)";
    case DecodeStatus::MfctPayload: return "manufacturer specific";
    case DecodeStatus::NoPayload: return "no payload";
    case DecodeStatus::BadHeader: return "unsupported";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Result helpers
// ---------------------------------------------------------------------------
static OutField* alloc_field(DecodeResult* r) {
    if (r->num_fields >= WMB_MAX_FIELDS) return nullptr;
    OutField* f = &r->fields[r->num_fields++];
    memset(f, 0, sizeof(*f));
    f->field_index = -1;
    f->text = "";
    return f;
}

static const char* pool_store(DecodeResult* r, const char* s) {
    if (!s || !s[0]) return "";
    size_t n = strlen(s) + 1;
    if (r->pool_used + n > sizeof(r->pool)) {
        // Out of pool space: keep a truncated copy in the remaining space.
        if ((size_t)r->pool_used + 2 > sizeof(r->pool)) return "";
        n = sizeof(r->pool) - r->pool_used;
    }
    char* p = r->pool + r->pool_used;
    memcpy(p, s, n - 1);
    p[n - 1] = 0;
    r->pool_used = (uint16_t)(r->pool_used + n);
    return p;
}

static OutField* set_numeric(DecodeResult* r, const char* vname, Quantity q, Unit u, double v,
                             int field_index, bool hidden, uint16_t offset) {
    OutField* f = nullptr;
    for (int i = 0; i < r->num_fields; ++i) {
        OutField* e = &r->fields[i];
        if (!e->is_text && e->unit == u && strcmp(e->vname, vname) == 0) { f = e; break; }
    }
    if (!f) {
        f = alloc_field(r);
        if (!f) return nullptr;
        snprintf(f->vname, sizeof(f->vname), "%s", vname);
        snprintf(f->name, sizeof(f->name), "%s_%s", vname, unit_suffix(u));
        f->field_index = (int16_t)field_index;
        f->offset = offset;
    }
    f->quantity = q;
    f->unit = u;
    f->value = v;
    f->is_text = false;
    f->hidden = hidden;
    f->text = "";
    return f;
}

static OutField* set_text(DecodeResult* r, const char* vname, const char* text, int field_index, bool hidden,
                          uint16_t offset) {
    OutField* f = nullptr;
    for (int i = 0; i < r->num_fields; ++i) {
        OutField* e = &r->fields[i];
        if (e->is_text && strcmp(e->vname, vname) == 0) { f = e; break; }
    }
    if (!f) {
        f = alloc_field(r);
        if (!f) return nullptr;
        snprintf(f->vname, sizeof(f->vname), "%s", vname);
        snprintf(f->name, sizeof(f->name), "%s", vname);
        f->field_index = (int16_t)field_index;
        f->offset = offset;
    }
    f->quantity = Quantity::Text;
    f->unit = Unit::TXT;
    f->value = NAN;
    f->is_text = true;
    f->hidden = hidden;
    if (!text || strcmp(f->text, text) != 0) f->text = pool_store(r, text);
    return f;
}

OutField* result_add_numeric(DecodeResult* r, const char* vname, Quantity q, Unit u, double v, int field_index, uint16_t offset) {
    return set_numeric(r, vname, q, u, v, field_index, false, offset);
}

OutField* result_add_text(DecodeResult* r, const char* vname, const char* text, int field_index, uint16_t offset) {
    return set_text(r, vname, text, field_index, false, offset);
}

const OutField* result_find(const DecodeResult* r, const char* name) {
    for (int i = 0; i < r->num_fields; ++i) if (strcmp(r->fields[i].name, name) == 0) return &r->fields[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Formula resolution
// ---------------------------------------------------------------------------
struct FCtx {
    const DecodeResult* r;
    const DvEntry* dve;
};

static bool fresolve(const char* ident, FVal* out, void* vctx) {
    FCtx* c = (FCtx*)vctx;
    if (c->dve) {
        if (!strcmp(ident, "storage_counter")) { *out = FVal{ (double)c->dve->storage, Unit::COUNTER, false, true }; return true; }
        if (!strcmp(ident, "tariff_counter")) { *out = FVal{ (double)c->dve->tariff, Unit::COUNTER, false, true }; return true; }
        if (!strcmp(ident, "subunit_counter")) { *out = FVal{ (double)c->dve->subunit, Unit::COUNTER, false, true }; return true; }
    }
    // Split "vname_unit": try each underscore from the right.
    size_t n = strlen(ident);
    for (size_t i = n; i-- > 0;) {
        if (ident[i] != '_') continue;
        Unit u = unit_from_suffix(ident + i + 1, n - i - 1);
        if (u == Unit::Unknown) continue;
        char vname[64];
        if (i >= sizeof(vname)) continue;
        memcpy(vname, ident, i);
        vname[i] = 0;
        const OutField* best = nullptr;
        for (int k = 0; k < c->r->num_fields; ++k) {
            const OutField* f = &c->r->fields[k];
            if (f->is_text || strcmp(f->vname, vname) != 0) continue;
            if (f->unit == u) { best = f; break; }
            if (!best && units_convertible(f->unit, u)) best = f;
        }
        if (!best) continue;
        double v = best->value;
        if (isnan(v)) return false;
        *out = FVal{ unit_convert(v, best->unit, u), u, false, true };
        return true;
    }
    return false;
}

static double eval_to_unit(const char* expr, const DecodeResult* r, const DvEntry* dve, Unit display, bool* ok) {
    FCtx c{ r, dve };
    FVal v = formula_eval(expr, fresolve, &c);
    *ok = v.ok;
    if (!v.ok) return NAN;
    if (!v.composite && v.u != Unit::Unknown && display != Unit::Unknown && units_convertible(v.u, display)) {
        return unit_convert(v.v, v.u, display);
    }
    return v.v;
}

static void fmt_date(double t, bool with_time, bool with_seconds, char* out, size_t out_max) {
    int y, mo, d, h, mi, s;
    unix_to_civil(t, &y, &mo, &d, &h, &mi, &s);
    if (with_seconds) snprintf(out, out_max, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    else if (with_time) snprintf(out, out_max, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
    else snprintf(out, out_max, "%04d-%02d-%02d", y, mo, d);
}

// Field name templates: "consumption_at_{storage_counter - 1 counter}".
static void expand_name(const char* pattern, const DecodeResult* r, const DvEntry* dve, char* out, size_t out_max) {
    size_t o = 0;
    for (const char* p = pattern; *p && o + 1 < out_max;) {
        if (*p == '{') {
            const char* close = strchr(p, '}');
            if (!close) break;
            char expr[96];
            size_t el = (size_t)(close - p - 1);
            if (el >= sizeof(expr)) el = sizeof(expr) - 1;
            memcpy(expr, p + 1, el);
            expr[el] = 0;
            char piece[32];
            if (!dve) {
                snprintf(piece, sizeof(piece), "{NULL}");
            } else {
                FCtx c{ r, dve };
                FVal v = formula_eval(expr, fresolve, &c);
                if (v.ok && !v.composite && unit_quantity(v.u) == Quantity::PointInTime) fmt_date(v.v, false, false, piece, sizeof(piece));
                else snprintf(piece, sizeof(piece), "%g", v.ok ? v.v : 0.0);
            }
            o += (size_t)snprintf(out + o, out_max - o, "%s", piece);
            if (o >= out_max) o = out_max - 1;
            p = close + 1;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------
static bool has_raw(const DvEntry* e, uint16_t raw) {
    if (e->named_combs_only) return false;
    for (int i = 0; i < e->ncomb; ++i) if (e->comb_raw[i] == raw) return true;
    return false;
}

static bool has_named(const DvEntry* e, uint8_t named) {
    for (int i = 0; i < e->ncomb; ++i) if ((uint8_t)combinable_from_raw(e->comb_raw[i]) == named) return true;
    return false;
}

static bool fm_matches(const FieldMatch& m, const DvEntry* e) {
    if (!(m.flags & FM_ACTIVE)) return false;
    if (m.flags & FM_DIFVIFKEY) return strcmp(e->key, m.difvifkey) == 0;
    if ((m.flags & FM_VIF_RANGE) && !vif_in_range(e->vif, m.vif_range)) return false;
    if ((m.flags & FM_VIF_RAW) && e->vif != m.vif_raw) return false;
    if ((m.flags & FM_MTYPE) && e->mtype != m.mtype) return false;
    if ((m.flags & FM_STORAGE) && (e->storage < m.storage_from || e->storage > m.storage_to)) return false;
    if ((m.flags & FM_TARIFF) && (e->tariff < m.tariff_from || e->tariff > m.tariff_to)) return false;
    if ((m.flags & FM_SUBUNIT) && (e->subunit < m.subunit_from || e->subunit > m.subunit_to)) return false;
    if (m.num_combs == 0 && m.num_combs_raw == 0 && !(m.flags & FM_COMB_ANY)) return e->ncomb == 0;
    for (int i = 0; i < m.num_combs_raw; ++i) if (!has_raw(e, m.combs_raw[i])) return false;
    for (int i = 0; i < m.num_combs; ++i) {
        if (m.combs[i] != (uint8_t)VifCombinable::Any && !has_named(e, m.combs[i])) return false;
    }
    if (!(m.flags & FM_COMB_ANY)) {
        if (m.num_combs > 0) {
            for (int i = 0; i < e->ncomb; ++i) {
                uint8_t named = (uint8_t)combinable_from_raw(e->comb_raw[i]);
                bool found = false;
                for (int j = 0; j < m.num_combs; ++j) if (m.combs[j] == named) { found = true; break; }
                if (!found) return false;
            }
        } else if (!e->named_combs_only) {
            for (int i = 0; i < e->ncomb; ++i) {
                bool found = false;
                for (int j = 0; j < m.num_combs_raw; ++j) if (m.combs_raw[j] == e->comb_raw[i]) { found = true; break; }
                if (!found) return false;
            }
        }
    }
    return true;
}

static bool fm_multi(const FieldMatch& m) {
    return ((m.flags & FM_STORAGE) && m.storage_from != m.storage_to) ||
           ((m.flags & FM_TARIFF) && m.tariff_from != m.tariff_to) ||
           ((m.flags & FM_SUBUNIT) && m.subunit_from != m.subunit_to);
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
struct Engine {
    Decoder* d;
    const DriverDef* drv;
    const uint8_t* key;
    const DvEntry* sorted[WMB_MAX_DV];
    int nsorted;
};

static void tpl_status_string(const Engine& en, char* out, size_t out_max) {
    uint8_t sts = en.d->t.tpl_sts;
    const LookupRule* rules = en.drv ? en.drv->tpl_status : nullptr;
    int nr = en.drv ? en.drv->num_tpl_status : 0;
    bool touches = false;
    for (int i = 0; i < nr; ++i) if (rules[i].mask & 0x1f) touches = true;
    if (nr > 0 && touches) {
        lookup_translate(rules, nr, sts, out, out_max);
        if (!out[0]) snprintf(out, out_max, "OK");
        return;
    }
    char s[96], t[96] = "OK";
    tpl_status_standard(sts, s, sizeof(s));
    if (sts & 0xe0) {
        if (nr > 0) lookup_translate(rules, nr, sts & 0xe0, t, sizeof(t));
        else snprintf(t, sizeof(t), "UNKNOWN_%02X", sts & 0xe0);
    }
    if (!strcmp(t, "OK") || !t[0]) { snprintf(out, out_max, "%s", s); return; }
    if (!strcmp(s, "OK") || !s[0]) { snprintf(out, out_max, "%s", t); return; }
    snprintf(out, out_max, "%s %s", s, t);
}

static void add_tpl_status(const Engine& en, char* existing, size_t cap) {
    char status[160];
    tpl_status_string(en, status, sizeof(status));
    if (strcmp(status, "OK") != 0) {
        if (strcmp(existing, "OK") != 0) {
            size_t l = strlen(existing);
            snprintf(existing + l, cap - l, "%s%s", l ? " " : "", status);
        } else {
            snprintf(existing, cap, "%s", status);
        }
    }
}

static bool text_range(VifRange r) {
    switch (r) {
    case VifRange::EnhancedIdentification: case VifRange::FabricationNo: case VifRange::HardwareVersion:
    case VifRange::FirmwareVersion: case VifRange::Medium: case VifRange::Manufacturer:
    case VifRange::ModelVersion: case VifRange::SoftwareVersion: case VifRange::Customer:
    case VifRange::Location: case VifRange::SpecialSupplierInformation: case VifRange::ParameterSet:
        return true;
    default: return false;
    }
}

static const DvEntry* find_key_with_nr(const Engine& en, const FieldMatch& m) {
    int nr = m.index_nr ? m.index_nr : 1;
    VifRange vr = (m.flags & FM_VIF_RANGE) ? m.vif_range : VifRange::Any;
    MeasurementType mt = (m.flags & FM_MTYPE) ? m.mtype : MeasurementType::Instantaneous;
    for (int i = 0; i < en.nsorted; ++i) {
        const DvEntry* e = en.sorted[i];
        if (!vif_in_range(e->vif, vr)) continue;
        if (!(mt == MeasurementType::Instantaneous || mt == e->mtype)) continue;
        if ((m.flags & FM_STORAGE) && e->storage != m.storage_from) continue;
        if ((m.flags & FM_TARIFF) && e->tariff != m.tariff_from) continue;
        if (--nr <= 0) return e;
    }
    return nullptr;
}

static void extract_string(Engine& en, const FieldDef& fi, int fidx, const DvEntry* dve) {
    DecodeResult* r = &en.d->res;
    bool hide = (fi.attrs & FATTR_HIDE) != 0;
    bool tpl = (fi.attrs & FATTR_INCLUDE_TPL_STATUS) != 0;
    bool has_matcher = (fi.match.flags & FM_ACTIVE) != 0;
    if (!dve) {
        if (!has_matcher) {
            if (tpl) {
                char s[288] = "OK";
                add_tpl_status(en, s, sizeof(s));
                set_text(r, fi.name, s, fidx, hide, 0);
            }
            return;
        }
        if (fi.match.flags & FM_DIFVIFKEY) {
            dve = dv_find(&en.d->dv, fi.match.difvifkey);
        } else {
            dve = find_key_with_nr(en, fi.match);
        }
        if (!dve) {
            if (tpl) {
                char s[288] = "OK";
                add_tpl_status(en, s, sizeof(s));
                set_text(r, fi.name, s, fidx, hide, 0);
            }
            return;
        }
    }
    char vname[48];
    expand_name(fi.name, r, dve, vname, sizeof(vname));
    VifRange vr = (fi.match.flags & FM_VIF_RANGE) ? fi.match.vif_range : VifRange::Any;
    char text[288];
    text[0] = 0;
    if (fi.num_lookups > 0 || tpl) {
        bool found = false;
        uint64_t bits = 0;
        if (fi.num_lookups > 0 && dv_extract_long(dve, &bits)) {
            lookup_translate(fi.lookups, fi.num_lookups, bits, text, sizeof(text));
            found = true;
        }
        if (tpl) add_tpl_status(en, text, sizeof(text));
        if (found) set_text(r, vname, text, fidx, hide, dve->offset);
    } else if (vr == VifRange::DateTime) {
        DvDate dd;
        if (dv_extract_date(dve, &dd)) {
            if (dve->data_len == 6)
                snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", dd.year, dd.month, dd.day, dd.hour, dd.minute, dd.second);
            else
                snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d", dd.year, dd.month, dd.day, dd.hour, dd.minute);
        }
        set_text(r, vname, text, fidx, hide, dve->offset);
    } else if (vr == VifRange::Date) {
        DvDate dd;
        if (dv_extract_date(dve, &dd)) snprintf(text, sizeof(text), "%04d-%02d-%02d", dd.year, dd.month, dd.day);
        set_text(r, vname, text, fidx, hide, dve->offset);
    } else if ((fi.flags & (FD_READABLE_NORMAL | FD_READABLE_REVERSED)) || text_range(vr)) {
        dv_extract_readable(dve, text, sizeof(text), (fi.flags & FD_READABLE_REVERSED) != 0);
        set_text(r, vname, text, fidx, hide, dve->offset);
    } else {
        dv_extract_hex(dve, text, sizeof(text));
        set_text(r, vname, text, fidx, hide, dve->offset);
    }
}

static void extract_numeric(Engine& en, const FieldDef& fi, int fidx, const DvEntry* dve) {
    if (!dve) return;
    DecodeResult* r = &en.d->res;
    bool hide = (fi.attrs & FATTR_HIDE) != 0;
    char vname[48];
    expand_name(fi.name, r, dve, vname, sizeof(vname));
    if (fi.calculate) {
        bool ok;
        double v = eval_to_unit(fi.calculate, r, dve, fi.display_unit, &ok);
        set_numeric(r, vname, fi.quantity, fi.display_unit, ok ? v : NAN, fidx, hide, dve->offset);
        return;
    }
    double v = NAN;
    if (!dv_extract_double(dve, &v, fi.vif_scaling == VifScaling::Auto, fi.signedness == DifSignedness::Unsigned)) return;
    Unit decoded = fi.display_unit;
    VifRange vr = (fi.match.flags & FM_VIF_RANGE) ? fi.match.vif_range : VifRange::Any;
    if (vr == VifRange::DateTime || vr == VifRange::Date) {
        DvDate dd;
        if (dv_extract_date(dve, &dd)) v = civil_to_unix(dd.year, dd.month, dd.day, dd.hour, dd.minute, dd.second);
        else v = NAN;
    } else if (vr == VifRange::AnyEnergyVIF || vr == VifRange::AnyVolumeVIF || vr == VifRange::AnyPowerVIF) {
        decoded = vif_range_default_unit(vif_to_range(dve->vif & 0x7f7f));
    } else if (vr != VifRange::Any && vr != VifRange::None) {
        decoded = vif_range_default_unit(vr);
    }
    if (fi.force_scale != 1.0) v *= fi.force_scale;
    if (unit_override_conversion(decoded, fi.display_unit)) decoded = fi.display_unit;
    if (decoded != Unit::Unknown && units_convertible(decoded, fi.display_unit)) v = unit_convert(v, decoded, fi.display_unit);
    if ((fi.flags & FD_HAS_NULL_VALUE) && v == fi.null_value) v = NAN;
    set_numeric(r, vname, fi.quantity, fi.display_unit, v, fidx, hide, dve->offset);
}

static void perform_extraction(Engine& en, const FieldDef& fi, int fidx, const DvEntry* dve) {
    if (fi.quantity == Quantity::Text) extract_string(en, fi, fidx, dve);
    else extract_numeric(en, fi, fidx, dve);
}

// ---------------------------------------------------------------------------
// ixml fields
// ---------------------------------------------------------------------------
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void add_ixml_captures(Engine& en, const IxmlResult& ix, uint16_t base) {
    for (int i = 0; i < ix.n; ++i) {
        const IxmlCapture& c = ix.caps[i];
        uint8_t bytes[24];
        size_t nb = 0;
        for (size_t k = 0; k + 1 < c.len && nb < sizeof(bytes); k += 2) {
            int hi = hexv(c.text[k]), lo = hexv(c.text[k + 1]);
            if (hi < 0 || lo < 0) break;
            bytes[nb++] = (uint8_t)(hi << 4 | lo);
        }
        dv_add_synthetic(&en.d->dv, c.dvk, bytes, nb, (uint16_t)(base + c.start / 2));
    }
}

static void add_decoding_error(Engine& en, const char* err, bool ok_join) {
    char* de = en.d->t.decoding_errors;
    char joined[sizeof(en.d->t.decoding_errors)];
    if (ok_join) status_join_ok(de, err, joined, sizeof(joined));
    else status_join_empty(de, err, joined, sizeof(joined));
    snprintf(de, sizeof(en.d->t.decoding_errors), "%s", joined);
}

// Qundis WalkByDataSet AES decode (manufacturer_specificities.cc).
static bool qundis_walkby_decode(Engine& en, char* hex, size_t cap) {
    size_t n = strlen(hex);
    if (n < 18) return false;
    if (hex[0] != '0' || hex[1] != '0' || hex[2] != '8' || hex[3] != '2' || hex[8] != '3' || hex[9] != '5') return false;
    if (!en.key) {
        // Encrypted block and no meter key: values stay undecoded.
        add_decoding_error(en, "MISSING_KEY", true);
        return false;
    }
    uint8_t acc = (uint8_t)(hexv(hex[4]) << 4 | hexv(hex[5]));
    uint8_t buf[128];
    size_t nb = 0;
    for (size_t k = 10; k + 1 < n && nb < sizeof(buf); k += 2) buf[nb++] = (uint8_t)(hexv(hex[k]) << 4 | hexv(hex[k + 1]));
    if (nb == 0 || nb % 16) return false;
    Telegram& t = en.d->t;
    uint8_t iv[16];
    int i = 0;
    if (t.tpl_id_found) {
        iv[i++] = t.tpl_mfct_b[0]; iv[i++] = t.tpl_mfct_b[1];
        for (int j = 0; j < 6; ++j) iv[i++] = t.tpl_a[j];
    } else {
        iv[i++] = t.dll_mfct_b[0]; iv[i++] = t.dll_mfct_b[1];
        for (int j = 0; j < 6; ++j) iv[i++] = t.dll_a[j];
    }
    for (int j = 0; j < 8; ++j) iv[i++] = acc;
    aes128_cbc_decrypt(en.key, iv, buf, buf, nb);
    hex[8] = '0';
    hex[9] = '0';
    static const char H[] = "0123456789ABCDEF";
    size_t o = 10;
    for (size_t k = 0; k < nb && o + 2 < cap; ++k) {
        hex[o++] = H[buf[k] >> 4];
        hex[o++] = H[buf[k] & 0xF];
    }
    hex[o] = 0;
    return true;
}

static void process_ixml_fields(Engine& en, const char* prios_hex) {
    Decoder* d = en.d;
    Telegram& t = d->t;
    for (int fidx = 0; fidx < en.drv->num_fields; ++fidx) {
        const FieldDef& fi = en.drv->fields[fidx];
        if (!fi.ixml) continue;
        bool required = (fi.attrs & FATTR_REQUIRED) != 0;
        char errname[64];
        snprintf(errname, sizeof(errname), "DECODING_ERROR_%s", fi.name);
        if (fi.flags & FD_MATCH_ENTIRE_FRAME) {
            bytes_to_hex(t.buf, t.len, d->hex, sizeof(d->hex));
            if (ixml_match(fi.ixml, d->hex, strlen(d->hex), &d->ix)) add_ixml_captures(en, d->ix, 0);
            else if (required) add_decoding_error(en, errname, false);
            continue;
        }
        if (fi.flags & FD_MATCH_ENTIRE_PAYLOAD) {
            if (prios_hex && prios_hex[0]) {
                snprintf(d->hex, sizeof(d->hex), "%s", prios_hex);
            } else {
                const uint8_t* p = t.payload();
                size_t n = t.payload_len();
                uint8_t tmp[WMB_FRAME_MAX];
                if (fi.flags & FD_TPL_AES_TRANSFORM) {
                    if (!en.key || fi.payload_offset < 0 || fi.tpl_acc_offset < 0) continue;
                    if ((size_t)fi.tpl_acc_offset >= n || (size_t)fi.payload_offset >= n) continue;
                    size_t end = n;
                    if (fi.payload_length > 0) {
                        end = (size_t)fi.payload_offset + (size_t)fi.payload_length;
                        if (end > n) continue;
                    }
                    size_t len = end - (size_t)fi.payload_offset;
                    memcpy(tmp, p + fi.payload_offset, len);
                    uint8_t saved_acc = t.tpl_acc;
                    t.tpl_acc = p[fi.tpl_acc_offset];
                    size_t dec = len - len % 16;
                    if (dec < 16) { t.tpl_acc = saved_acc; continue; }
                    uint8_t iv[16];
                    int i = 0;
                    if (t.tpl_id_found) {
                        iv[i++] = t.tpl_mfct_b[0]; iv[i++] = t.tpl_mfct_b[1];
                        for (int j = 0; j < 6; ++j) iv[i++] = t.tpl_a[j];
                    } else {
                        iv[i++] = t.dll_mfct_b[0]; iv[i++] = t.dll_mfct_b[1];
                        for (int j = 0; j < 6; ++j) iv[i++] = t.dll_a[j];
                    }
                    for (int j = 0; j < 8; ++j) iv[i++] = t.tpl_acc;
                    aes128_cbc_decrypt(en.key, iv, tmp, tmp, dec);
                    p = tmp;
                    n = len;
                }
                bytes_to_hex(p, n, d->hex, sizeof(d->hex));
            }
            if (ixml_match(fi.ixml, d->hex, strlen(d->hex), &d->ix)) add_ixml_captures(en, d->ix, t.header_size);
            else if (required) add_decoding_error(en, errname, false);
            continue;
        }
        if (!(fi.match.flags & FM_ACTIVE)) continue;
        int n = d->dv.n;
        for (int i = 0; i < n; ++i) {
            const DvEntry* e = &d->dv.e[i];
            if (!fm_matches(fi.match, e)) continue;
            perform_extraction(en, fi, fidx, e);
            dv_extract_hex(e, d->hex, sizeof(d->hex));
            bool extra = (en.drv->flags & DRV_TRY_QUNDIS_DECODE) && qundis_walkby_decode(en, d->hex, sizeof(d->hex));
            uint16_t base = e->offset;
            if (ixml_match(fi.ixml, d->hex, strlen(d->hex), &d->ix)) {
                add_ixml_captures(en, d->ix, base);
            } else {
                // AES-CBC has no integrity check: a wrong key decrypts into
                // garbage that the grammar rejects.
                if (extra) add_decoding_error(en, "FAILED_DECODE", true);
                if (required) add_decoding_error(en, errname, false);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Diehl PRIOS (izarv2): decode LFSR payload, inject SAP PRIOS strings.
// ---------------------------------------------------------------------------
static bool diehl_prios(Engine& en, char* combined_hex, size_t cap) {
    Telegram& t = en.d->t;
    uint32_t keys[3];
    int nk = 0;
    if (en.key) keys[nk++] = diehl_convert_key(en.key);
    if (nk == 0) { diehl_default_keys(keys); nk = 2; }
    const uint8_t* origin = t.has_original ? t.original : t.buf;
    uint8_t dec[WMB_FRAME_MAX];
    size_t n = 0;
    for (int k = 0; k < nk && !n; ++k) n = diehl_lfsr_decode(origin, t.buf, t.len, keys[k], true, 0x4B, dec);
    if (!n) return false;
    uint8_t comb[WMB_FRAME_MAX];
    size_t hs = t.header_size;
    size_t take = hs + 4 <= t.len ? 4 : (t.len > hs ? t.len - hs : 0);
    memcpy(comb, t.buf + hs, take);
    memcpy(comb + take, dec, n);
    bytes_to_hex(comb, take + n, combined_hex, cap);
    if (diehl_frame_interpretation(t.buf, t.len) == DiehlFrame::SAP_PRIOS) {
        const uint8_t* o = origin;
        uint32_t num = ((uint32_t)(o[7] & 0x03) << 24) | ((uint32_t)o[6] << 16) | ((uint32_t)o[5] << 8) | (uint32_t)o[4];
        char digits[16];
        snprintf(digits, sizeof(digits), "%08lu", (unsigned long)num);
        int yy = (digits[0] - '0') * 10 + (digits[1] - '0');
        int year = yy > 70 ? 1900 + yy : 2000 + yy;
        unsigned long serial = strtoul(digits + 2, nullptr, 10);
        char supplier = (char)('@' + (((o[9] & 0x0F) << 1) | (o[8] >> 7)));
        char mtype = (char)('@' + ((o[8] & 0x7C) >> 2));
        char diam = (char)('@' + (((o[8] & 0x03) << 3) | (o[7] >> 5)));
        char buf[32];
        DecodeResult* r = &en.d->res;
        snprintf(buf, sizeof(buf), "%c%02d%c%c", supplier, yy, mtype, diam);
        set_text(r, "prefix", buf, -1, false, 0);
        snprintf(buf, sizeof(buf), "%06lu", serial);
        set_text(r, "serial_number", buf, -1, false, 0);
        snprintf(buf, sizeof(buf), "%d", year);
        set_text(r, "manufacture_y", buf, -1, false, 0);
        // Order these like the driver fields when they exist.
        for (int i = 0; i < en.drv->num_fields; ++i) {
            const char* nm = en.drv->fields[i].name;
            for (int k = 0; k < r->num_fields; ++k) {
                if (r->fields[k].field_index == -1 && !strcmp(r->fields[k].vname, nm)) r->fields[k].field_index = (int16_t)i;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Driver run
// ---------------------------------------------------------------------------
void engine_run_driver(Decoder* d, const DriverDef* drv, const uint8_t* key) {
    static Engine en;
    en.d = d;
    en.drv = drv;
    en.key = key;
    DecodeResult* r = &d->res;

    static char prios_hex[2 * WMB_FRAME_MAX + 2];
    prios_hex[0] = 0;
    if (drv->flags & DRV_DIEHL_PRIOS) {
        if (!diehl_prios(en, prios_hex, sizeof(prios_hex))) prios_hex[0] = 0;
    }

    process_ixml_fields(en, prios_hex);

    // Sort DV entries by offset (stable).
    en.nsorted = 0;
    for (int i = 0; i < d->dv.n; ++i) {
        const DvEntry* e = &d->dv.e[i];
        int j = en.nsorted++;
        while (j > 0 && en.sorted[j - 1]->offset > e->offset) { en.sorted[j] = en.sorted[j - 1]; j--; }
        en.sorted[j] = e;
    }

    static bool found[256];
    memset(found, 0, sizeof(found));
    for (int fidx = 0; fidx < drv->num_fields; ++fidx) {
        const FieldDef& fi = drv->fields[fidx];
        if (fi.ixml) continue;
        if (!(fi.match.flags & FM_ACTIVE)) continue;
        bool multi = fm_multi(fi.match);
        int nr = fi.match.index_nr ? fi.match.index_nr : 1;
        int current = 0;
        for (int i = 0; i < en.nsorted; ++i) {
            const DvEntry* e = en.sorted[i];
            if (!fm_matches(fi.match, e)) continue;
            current++;
            if (nr != current && !multi) continue;
            perform_extraction(en, fi, fidx, e);
            found[fidx] = true;
        }
    }
    for (int fidx = 0; fidx < drv->num_fields; ++fidx) {
        const FieldDef& fi = drv->fields[fidx];
        if (fi.ixml) continue;
        if (!(fi.match.flags & FM_ACTIVE)) {
            if (!fi.calculate) perform_extraction(en, fi, fidx, nullptr);
        } else if (!found[fidx] && (fi.attrs & FATTR_INCLUDE_TPL_STATUS)) {
            perform_extraction(en, fi, fidx, nullptr);
        }
    }
    // Calculators (formula without matcher), in field order.
    for (int fidx = 0; fidx < drv->num_fields; ++fidx) {
        const FieldDef& fi = drv->fields[fidx];
        if (!fi.calculate || (fi.match.flags & FM_ACTIVE)) continue;
        bool ok;
        double v = eval_to_unit(fi.calculate, r, nullptr, fi.display_unit, &ok);
        set_numeric(r, fi.name, fi.quantity, fi.display_unit, ok ? v : NAN, fidx, (fi.attrs & FATTR_HIDE) != 0, 0xFFFF);
    }
    // The status field joins all INJECT_INTO_STATUS fields.
    for (int fidx = 0; fidx < drv->num_fields; ++fidx) {
        const FieldDef& fi = drv->fields[fidx];
        if (!(fi.attrs & FATTR_STATUS) || fi.quantity != Quantity::Text) continue;
        OutField* sf = nullptr;
        for (int i = 0; i < r->num_fields; ++i) if (r->fields[i].is_text && !strcmp(r->fields[i].vname, fi.name)) sf = &r->fields[i];
        char value[288];
        snprintf(value, sizeof(value), "%s", sf ? sf->text : "null");
        bool any_inject = false;
        for (int k = 0; k < drv->num_fields; ++k) {
            const FieldDef& fk = drv->fields[k];
            if (!(fk.attrs & FATTR_INJECT_INTO_STATUS)) continue;
            any_inject = true;
            const char* more = "null";
            for (int i = 0; i < r->num_fields; ++i) if (r->fields[i].is_text && !strcmp(r->fields[i].vname, fk.name)) more = r->fields[i].text;
            char joined[288];
            status_join_ok(value, more, joined, sizeof(joined));
            snprintf(value, sizeof(value), "%s", joined);
        }
        if (!sf && !any_inject) break; // stays null
        status_sort(value);
        if (!value[0]) snprintf(value, sizeof(value), "OK");
        if (d->t.decoding_errors[0]) {
            // Decoding problems are appended after sorting, like upstream.
            char joined[288];
            status_join_ok(value, d->t.decoding_errors, joined, sizeof(joined));
            snprintf(value, sizeof(value), "%s", joined);
        }
        sf = set_text(r, fi.name, value, fidx, (fi.attrs & FATTR_HIDE) != 0, 0);
        if (sf) sf->is_status = true;
        break;
    }
    // Order the output like the driver's field list.
    for (int i = 1; i < r->num_fields; ++i) {
        OutField tmp = r->fields[i];
        int j = i;
        while (j > 0 && (uint16_t)r->fields[j - 1].field_index > (uint16_t)tmp.field_index) {
            r->fields[j] = r->fields[j - 1];
            j--;
        }
        r->fields[j] = tmp;
    }
}

} // namespace wmb
