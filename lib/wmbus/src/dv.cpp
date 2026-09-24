// wM-Buster ADV — DIF/VIF record parsing and value extraction.
// GPL-3.0
#include "wmbus/dv.h"
#include "wmbus/crc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace wmb {

static const char HEXU[] = "0123456789ABCDEF";

void dv_clear(DvSet* s) {
    s->n = 0;
    s->arena_used = 0;
    s->mfct_0f_index = -1;
    s->format_hash = 0;
    s->format_len = 0;
    s->truncated = false;
}

static int dif_len_bytes(uint8_t dif) {
    switch (dif & 0x0f) {
    case 0x0: return 0;
    case 0x1: return 1;
    case 0x2: return 2;
    case 0x3: return 3;
    case 0x4: return 4;
    case 0x5: return 4;
    case 0x6: return 6;
    case 0x7: return 8;
    case 0x8: return 0;
    case 0x9: return 1;
    case 0xA: return 2;
    case 0xB: return 3;
    case 0xC: return 4;
    case 0xD: return -1;
    case 0xE: return 6;
    default: return dif == 0x2f ? 1 : -2;
    }
}

static MeasurementType dif_mtype(uint8_t dif) {
    switch (dif & 0x30) {
    case 0x00: return MeasurementType::Instantaneous;
    case 0x10: return MeasurementType::Maximum;
    case 0x20: return MeasurementType::Minimum;
    default: return MeasurementType::AtError;
    }
}

static const uint8_t* arena_store(DvSet* s, const uint8_t* src, size_t n) {
    if (s->arena_used + n > sizeof(s->arena)) {
        s->truncated = true;
        return nullptr;
    }
    uint8_t* p = s->arena + s->arena_used;
    if (n) memcpy(p, src, n);
    s->arena_used += n;
    return p;
}

static int count_key(const DvSet* s, const char* base, size_t blen) {
    int c = 0;
    for (int i = 0; i < s->n; ++i) {
        const char* k = s->e[i].key;
        if (strncmp(k, base, blen) == 0 && (k[blen] == 0 || k[blen] == '_')) c++;
    }
    return c;
}

static DvEntry* new_entry(DvSet* s) {
    if (s->n >= WMB_MAX_DV) { s->truncated = true; return nullptr; }
    DvEntry* e = &s->e[s->n];
    memset(e, 0, sizeof(*e));
    return e;
}

// ---------------------------------------------------------------------------
// Compact profiles (EN 13757-3 Annex F). Like upstream, every point of a
// profile becomes a synthetic entry with its own storage number (base storage
// + 1, + 2, ...) and the wmbusmeters specific Synthetic combinable (7F77), so
// that drivers can match the history with storage ranges. Dates and actuality
// durations are generated for the points when the base has them.
// ---------------------------------------------------------------------------
enum class CpDist : uint8_t { Unknown, NotSpaced, Seconds, Minutes, Hours, Days, HalfMonth, OneMonth, ThreeMonths, SixMonths };
enum { CP_ABSOLUTE = 0, CP_INCREMENTS = 1, CP_DECREMENTS = 2, CP_SIGNED_DIFFERENCE = 3 };
static const uint16_t RAW_SYNTHETIC = 0x7f77;

static bool cp_is_binary(int n) { return n == 1 || n == 2 || n == 3 || n == 4 || n == 6 || n == 7; }
static bool cp_is_bcd(int n) { return n == 9 || n == 0xA || n == 0xB || n == 0xC || n == 0xE; }
static bool cp_marker(uint16_t raw) { return raw == 0x13 || raw == 0x1E || raw == 0x1F; }

static bool cp_header(uint8_t sc, uint8_t sv, uint8_t* nib, int* mode, CpDist* dist, bool* uns, int* step, int* col) {
    *nib = sc & 0x0f;
    *mode = (sc >> 6) & 0x03;
    *dist = CpDist::Unknown;
    // Annex F.2.4 note 1: binary values are unsigned for modes 01b and 10b.
    *uns = cp_is_binary(*nib) && (*mode == CP_INCREMENTS || *mode == CP_DECREMENTS);
    *step = 0;
    *col = 0;
    uint8_t unit = (sc >> 4) & 0x03;
    if (sv == 0) {
        *dist = CpDist::NotSpaced;
        *col = unit + 1;  // Table F.8 note a: up to four columns
        return true;
    }
    if (sv == 251 || sv == 252 || sv == 255) return false;
    if (sv <= 250) {
        static const CpDist UNITS[4] = { CpDist::Seconds, CpDist::Minutes, CpDist::Hours, CpDist::Days };
        *step = sv;
        *dist = UNITS[unit];
        return true;
    }
    if (sv == 253) {
        if (unit != 3) return false;
        *dist = CpDist::HalfMonth;
        *step = 1;
        return true;
    }
    if (unit == 1) { *dist = CpDist::SixMonths; *step = 6; return true; }
    if (unit == 2) { *dist = CpDist::ThreeMonths; *step = 3; return true; }
    if (unit == 3) { *dist = CpDist::OneMonth; *step = 1; return true; }
    return false;
}

static bool has_raw_comb(const DvEntry* e, uint16_t raw) {
    for (int i = 0; i < e->ncomb; ++i) if (e->comb_raw[i] == raw) return true;
    return false;
}

// Named combinables of a contain those of b, profile markers ignored.
static bool cp_named_subset(const DvEntry* a, const DvEntry* b) {
    for (int i = 0; i < b->ncomb; ++i) {
        if (cp_marker(b->comb_raw[i])) continue;
        VifCombinable nb = combinable_from_raw(b->comb_raw[i]);
        bool found = false;
        for (int j = 0; j < a->ncomb && !found; ++j)
            found = !cp_marker(a->comb_raw[j]) && combinable_from_raw(a->comb_raw[j]) == nb;
        if (!found) return false;
    }
    return true;
}

static bool cp_same_combs(const DvEntry* a, const DvEntry* b) { return cp_named_subset(a, b) && cp_named_subset(b, a); }

// Base value of the profile: same storage, vif, tariff and subunit. Base and
// increments may use different codings (OMS Vol.2 Annex G, table G.4).
static bool cp_base_value(const DvSet* s, const DvEntry* p, uint8_t* nib, uint64_t* v) {
    for (int exact = 1; exact >= 0; --exact) {
        for (int i = 0; i < s->n; ++i) {
            const DvEntry* c = &s->e[i];
            if ((c->dif & 0x0f) == 0x0d) continue;
            if (exact && !cp_same_combs(c, p)) continue;
            if (c->storage != p->storage || (c->vif & 0xff) != (p->vif & 0xff)) continue;
            if (c->tariff != p->tariff || c->subunit != p->subunit) continue;
            if (dv_extract_long(c, v)) {
                *nib = c->dif & 0x0f;
                return true;
            }
        }
    }
    return false;
}

// Actuality duration of the base value (OMS Vol.2 Annex R.3.2), in seconds.
static bool cp_base_actuality(const DvSet* s, const DvEntry* p, double* secs) {
    for (int i = 0; i < s->n; ++i) {
        const DvEntry* c = &s->e[i];
        if (c->storage != p->storage || c->tariff != p->tariff || c->subunit != p->subunit) continue;
        if (!vif_in_range(c->vif, VifRange::ActualityDuration)) continue;
        if (has_raw_comb(c, RAW_SYNTHETIC)) continue;
        double hours;
        if (!dv_extract_double(c, &hours, true, true)) continue;
        *secs = hours * 3600.0;
        return true;
    }
    return false;
}

static const DvEntry* cp_base_date(const DvSet* s, const DvEntry* p, DvDate* d) {
    for (int i = 0; i < s->n; ++i) {
        const DvEntry* c = &s->e[i];
        if (c->storage != p->storage || c->tariff != p->tariff || c->subunit != p->subunit) continue;
        if (!vif_in_range(c->vif, VifRange::Date)) continue;
        if (!dv_extract_date(c, d)) continue;
        if (c->data_len != 2 && c->data_len != 4) continue;
        return c;
    }
    return nullptr;
}

static int days_in_month(int y, int m) {
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return D[(m - 1) % 12];
}

static void cp_add_months(DvDate* d, int months) {
    bool last = d->day == days_in_month(d->year, d->month);
    int m0 = d->month - 1 + months % 12;
    int y = d->year + months / 12;
    while (m0 > 11) { y++; m0 -= 12; }
    while (m0 < 0) { y--; m0 += 12; }
    int dim = days_in_month(y, m0 + 1);
    d->year = y;
    d->month = m0 + 1;
    d->day = last ? dim : (d->day < dim ? d->day : dim);
}

static bool cp_step_date(DvDate* d, CpDist dist, int step, bool inverse) {
    int dir = inverse ? -1 : 1;
    long long secs = 0;
    switch (dist) {
    case CpDist::OneMonth: case CpDist::ThreeMonths: case CpDist::SixMonths:
        cp_add_months(d, dir * (step > 0 ? step : 1));
        return true;
    case CpDist::HalfMonth:
        // Calendar half months: the 1st and the 16th.
        if (inverse) {
            if (d->day >= 16) d->day = 1;
            else { cp_add_months(d, -1); d->day = 16; }
        } else {
            if (d->day <= 15) d->day = 16;
            else { cp_add_months(d, 1); d->day = 1; }
        }
        return true;
    case CpDist::Seconds: secs = step; break;
    case CpDist::Minutes: secs = (long long)step * 60; break;
    case CpDist::Hours: secs = (long long)step * 3600; break;
    case CpDist::Days: secs = (long long)step * 86400; break;
    default: return false;
    }
    if (secs <= 0) return false;
    double t = civil_to_unix(d->year, d->month, d->day, d->hour, d->minute, d->second) + dir * (double)secs;
    unix_to_civil(t, &d->year, &d->month, &d->day, &d->hour, &d->minute, &d->second);
    return true;
}

static int cp_encode_date(const DvDate& d, int len, uint8_t* out) {
    if (d.year < 2000 || d.year > 2127 || d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31) return 0;
    int y = d.year - 2000;
    uint8_t lo = (uint8_t)((d.day & 0x1f) | ((y & 0x07) << 5));
    uint8_t hi = (uint8_t)((d.month & 0x0f) | ((y & 0x78) << 1));
    if (len == 2) { out[0] = lo; out[1] = hi; return 2; }
    if (len != 4 || d.hour < 0 || d.hour > 23 || d.minute < 0 || d.minute > 59) return 0;
    out[0] = (uint8_t)(d.minute & 0x3f);
    out[1] = (uint8_t)(d.hour & 0x1f);
    out[2] = lo;
    out[3] = hi;
    return 4;
}

static bool cp_decode_signed(int nib, const uint8_t* b, int n, bool uns, int64_t* out) {
    if (cp_is_bcd(nib)) {
        uint64_t v = 0, mul = 1;
        for (int i = 0; i < n; ++i) {
            int lo = b[i] & 0x0f, hi = b[i] >> 4;
            if (lo > 9 || hi > 9) return false;
            v += (uint64_t)lo * mul; mul *= 10;
            v += (uint64_t)hi * mul; mul *= 10;
        }
        if (v > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)v;
        return true;
    }
    if (!cp_is_binary(nib) || n <= 0 || n > 8) return false;
    uint64_t raw = 0;
    for (int i = 0; i < n; ++i) raw |= (uint64_t)b[i] << (8 * i);
    if (uns) {
        if (raw > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)raw;
        return true;
    }
    if (n < 8) {
        uint64_t sign = (uint64_t)1 << (n * 8 - 1), mask = ((uint64_t)1 << (n * 8)) - 1;
        raw &= mask;
        if (raw & sign) raw |= ~mask;
    }
    *out = (int64_t)raw;
    return true;
}

static bool cp_encode_signed(int nib, int n, int64_t v, uint8_t* out) {
    if (cp_is_bcd(nib)) {
        if (v < 0) return false;
        uint64_t u = (uint64_t)v;
        for (int i = 0; i < n; ++i) {
            uint8_t lo = (uint8_t)(u % 10); u /= 10;
            uint8_t hi = (uint8_t)(u % 10); u /= 10;
            out[i] = (uint8_t)(hi << 4 | lo);
        }
        return u == 0;
    }
    if (!cp_is_binary(nib) || n <= 0 || n > 8) return false;
    if (n < 8) {
        int64_t lo = -((int64_t)1 << (n * 8 - 1)), hi = ((int64_t)1 << (n * 8 - 1)) - 1;
        if (v < lo || v > hi) return false;
    }
    for (int i = 0; i < n; ++i) out[i] = (uint8_t)(((uint64_t)v >> (8 * i)) & 0xff);
    return true;
}

// Key like upstream makeSyntheticStorageKey: dif(+difes) vif 7F77 [_n].
static uint8_t cp_key(const DvSet* s, uint8_t nib, int storage, uint8_t vif, char* out, size_t cap) {
    uint8_t b[12];
    size_t n = 0;
    int rem = storage >> 1;
    uint8_t dif = (uint8_t)((nib & 0x0f) | ((storage & 1) ? 0x40 : 0x00) | (rem > 0 ? 0x80 : 0x00));
    b[n++] = dif;
    while (rem > 0 && n < 8) {
        uint8_t dife = rem & 0x0f;
        rem >>= 4;
        if (rem > 0) dife |= 0x80;
        b[n++] = dife;
    }
    b[n++] = vif;
    b[n++] = 0x7f;
    b[n++] = 0x77;
    size_t kl = 0;
    for (size_t i = 0; i < n && kl + 2 < cap; ++i) {
        out[kl++] = HEXU[b[i] >> 4];
        out[kl++] = HEXU[b[i] & 0xF];
    }
    out[kl] = 0;
    for (int dup = 2; dv_find(s, out); ++dup) snprintf(out + kl, cap - kl, "_%d", dup);
    return dif;
}

static DvEntry* cp_add(DvSet* s, uint8_t nib, int storage, uint16_t vif, MeasurementType mt, const DvEntry* p,
                       uint16_t subunit, const uint8_t* val, size_t n) {
    if (s->n >= WMB_MAX_DV) { s->truncated = true; return nullptr; }
    const uint8_t* stored = arena_store(s, val, n);
    if (!stored) return nullptr;
    DvEntry* e = &s->e[s->n];
    memset(e, 0, sizeof(*e));
    e->dif = cp_key(s, nib, storage, (uint8_t)(vif & 0xff), e->key, sizeof(e->key));
    e->offset = p->offset;
    e->vif = vif;
    e->mtype = mt;
    e->storage = (uint32_t)storage;
    e->tariff = p->tariff;
    e->subunit = subunit;
    e->data = stored;
    e->data_len = (uint8_t)n;
    e->synthetic = true;
    e->named_combs_only = true;
    s->n++;
    return e;
}

static void cp_set_combs(DvEntry* e, const DvEntry* from) {
    e->ncomb = 0;
    if (from) {
        for (int i = 0; i < from->ncomb; ++i)
            if (!cp_marker(from->comb_raw[i]) && e->ncomb < WMB_DV_MAX_COMB - 1) e->comb_raw[e->ncomb++] = from->comb_raw[i];
    }
    e->comb_raw[e->ncomb++] = RAW_SYNTHETIC;
}

static void cp_expand(DvSet* s, int idx) {
    const DvEntry* p = &s->e[idx];
    bool compact = has_raw_comb(p, 0x1F), with_register = has_raw_comb(p, 0x1E), inverse = has_raw_comb(p, 0x13);
    if (!compact && !with_register && !inverse) return;
    const uint8_t* pl = p->data;
    size_t plen = p->data_len;
    if (plen < 2) return;

    uint8_t nib;
    int mode, step, column;
    CpDist dist;
    bool uns;
    if (!cp_header(pl[0], pl[1], &nib, &mode, &dist, &uns, &step, &column)) return;
    int slot = dif_len_bytes(nib);
    if (slot <= 0 || (plen - 2) % (size_t)slot != 0) return;

    uint8_t base_nib = nib;
    uint64_t base_u = 0;
    bool have_base = cp_base_value(s, p, &base_nib, &base_u);
    int64_t running = (int64_t)base_u;
    int base_bytes = dif_len_bytes(base_nib);
    if (base_bytes <= 0) have_base = false;

    int spacing_s = 0;
    switch (dist) {
    case CpDist::Seconds: spacing_s = step; break;
    case CpDist::Minutes: spacing_s = step * 60; break;
    case CpDist::Hours: spacing_s = step * 3600; break;
    case CpDist::Days: spacing_s = step * 86400; break;
    default: break;
    }
    double base_age = 0;
    bool have_age = cp_base_actuality(s, p, &base_age);

    DvDate date;
    const DvEntry* date_e = cp_base_date(s, p, &date);
    uint8_t date_nib = date_e ? (date_e->dif & 0x0f) : 0;
    uint8_t date_vif = date_e ? (uint8_t)(date_e->vif & 0xff) : 0;
    MeasurementType date_mt = date_e ? date_e->mtype : MeasurementType::Instantaneous;
    int date_len = date_e ? date_e->data_len : 0;

    // Copy what we need: adding entries does not move s->e, but keep the
    // profile's identity explicit.
    DvEntry prof = *p;
    bool incremental = mode != CP_ABSOLUTE;
    int synth = 0;
    int nslots = (int)((plen - 2) / (size_t)slot);
    bool forward = inverse || with_register;
    for (int k = 0; k < nslots; ++k) {
        size_t off = forward ? 2 + (size_t)k * slot : 2 + (size_t)(nslots - 1 - k) * slot;
        const uint8_t* v = pl + off;
        bool all_ff = true;
        for (int i = 0; i < slot; ++i) if (v[i] != 0xff) { all_ff = false; break; }
        if (all_ff) {
            if (incremental) break;  // invalid values end incremental profiles
            continue;
        }
        if (incremental && cp_is_binary(nib) && !uns) {
            bool illegal = v[slot - 1] == 0x80;  // most negative value is "illegal"
            for (int i = 0; i + 1 < slot && illegal; ++i) illegal = v[i] == 0x00;
            if (illegal) break;
        }
        bool reconstruct = inverse && incremental && have_base;
        uint8_t val_nib = reconstruct ? base_nib : nib;
        uint8_t val[8];
        int val_len = slot;
        if (reconstruct) {
            int64_t delta;
            if (!cp_decode_signed(nib, v, slot, uns, &delta)) break;
            int64_t absolute = running;
            if (mode == CP_DECREMENTS) absolute = running + delta;
            else absolute = running - delta;  // increments, signed difference (younger - older)
            if (!cp_encode_signed(val_nib, base_bytes, absolute, val)) break;
            val_len = base_bytes;
            running = absolute;
        } else {
            memcpy(val, v, (size_t)slot > sizeof(val) ? sizeof(val) : (size_t)slot);
            if (val_len > (int)sizeof(val)) val_len = sizeof(val);
        }
        int storage = (int)prof.storage + 1 + synth;
        uint16_t subunit = prof.subunit;
        if (dist == CpDist::NotSpaced && column > 0) subunit = (uint16_t)(subunit + column - 1);

        DvEntry* e = cp_add(s, val_nib, storage, prof.vif, prof.mtype, &prof, subunit, val, (size_t)val_len);
        if (!e) return;
        cp_set_combs(e, &prof);

        if (date_e) {
            DvDate next = date;
            if (cp_step_date(&next, dist, step, inverse)) {
                uint8_t db[4];
                int dn = cp_encode_date(next, date_len, db);
                if (dn) {
                    DvEntry* de = cp_add(s, date_nib, storage, date_vif, date_mt, &prof, subunit, db, (size_t)dn);
                    if (!de) return;
                    cp_set_combs(de, nullptr);
                    date = next;
                }
            }
        } else if (spacing_s > 0 && have_age) {
            // No base time (usual for TAF7 profiles): give every point its own
            // actuality duration so a receiver with a clock can place it.
            uint32_t age = (uint32_t)lround(base_age) + (uint32_t)spacing_s * (uint32_t)(synth + 1);
            uint8_t ab[4] = { (uint8_t)age, (uint8_t)(age >> 8), (uint8_t)(age >> 16), (uint8_t)(age >> 24) };
            DvEntry* ae = cp_add(s, 0x4, storage, 0x74, prof.mtype, &prof, subunit, ab, 4);
            if (!ae) return;
            cp_set_combs(ae, &prof);
        }
        synth++;
    }
}

void dv_parse(DvSet* s, const uint8_t* data, size_t len, uint16_t base_offset,
              const uint8_t* format, size_t format_len) {
    const uint8_t* d = data;
    const uint8_t* dend = data + len;
    bool has_difvifs = format == nullptr;
    const uint8_t* f = has_difvifs ? data : format;
    const uint8_t* fend = has_difvifs ? dend : format + format_len;
    uint8_t fmt_bytes[64];
    size_t fmt_len = 0;
    bool fmt_overflow = false;

    // In the normal case the format pointer IS the data pointer.
#define ADV_F() do { if (has_difvifs) { d++; f = d; } else { f++; } } while (0)
#define FMT_PUSH(b) do { if (has_difvifs) { if (fmt_len < sizeof(fmt_bytes)) fmt_bytes[fmt_len++] = (b); else fmt_overflow = true; } } while (0)

    for (;;) {
        if (has_difvifs) f = d;
        if (f >= fend) break;
        uint8_t id_bytes[24];
        size_t nid = 0;
#define ID_PUSH(b) do { if (nid < sizeof(id_bytes)) id_bytes[nid++] = (b); } while (0)

        uint8_t dif = *f;
        MeasurementType mt = dif_mtype(dif);
        int datalen = dif_len_bytes(dif);

        if (datalen == -2) {
            if (!has_difvifs) break;
            size_t idx = (size_t)(d - data);
            s->mfct_0f_index = (int)(1 + idx);
            if (dif == 0x0F) {
                DvEntry* e = new_entry(s);
                if (e) {
                    strcpy(e->key, "0F");
                    e->offset = (uint16_t)(base_offset + idx);
                    e->dif = 0x0F;
                    e->vif = 0x7f;
                    e->mtype = MeasurementType::Instantaneous;
                    e->data = d + 1;
                    size_t n = (size_t)(dend - d - 1);
                    e->data_len = (uint8_t)(n > 255 ? 255 : n);
                    s->n++;
                }
            }
            break;
        }
        if (dif == 0x2f) {
            ADV_F();
            continue;
        }
        bool variable_length = datalen == -1;
        FMT_PUSH(dif);
        ID_PUSH(dif);
        ADV_F();

        int difenr = 0, subunit = 0, tariff = 0;
        uint32_t storage = (dif & 0x40) >> 6;
        bool more = (dif & 0x80) != 0;
        int num_dife = 0;
        while (more) {
            num_dife++;
            if (num_dife > 10) break;
            if (f >= fend) break;
            uint8_t dife = *f;
            subunit |= ((dife & 0x40) >> 6) << difenr;
            tariff |= ((dife & 0x30) >> 4) << (difenr * 2);
            storage |= (uint32_t)(dife & 0x0f) << (1 + difenr * 4);
            FMT_PUSH(dife);
            ID_PUSH(dife);
            ADV_F();
            more = (dife & 0x80) != 0;
            difenr++;
        }
        if (f >= fend) break;

        uint8_t vif = *f;
        uint16_t full_vif = vif & 0x7f;
        bool extension_vif = false;
        uint16_t comb_full = 0;
        bool comb_ext = false;
        uint16_t combs[WMB_DV_MAX_COMB];
        int ncomb = 0;
        FMT_PUSH(vif);
        ID_PUSH(vif);
        ADV_F();
        if (vif == 0xfb || vif == 0xfd || vif == 0xef || vif == 0xff) {
            full_vif <<= 8;
            extension_vif = true;
        }
        if (vif == 0x7c || vif == 0xfc) {
            if (f >= fend) break;
            uint8_t viflen = *f;
            ID_PUSH(viflen);
            ADV_F();
            for (uint8_t i = 0; i < viflen; ++i) {
                if (f >= fend) break;
                ID_PUSH(*f);
                ADV_F();
            }
        }
        more = (vif & 0x80) != 0;
        int num_vife = 0;
        while (more) {
            num_vife++;
            if (num_vife > 10) break;
            if (f >= fend) break;
            uint8_t vife = *f;
            FMT_PUSH(vife);
            ID_PUSH(vife);
            ADV_F();
            more = (vife & 0x80) != 0;
            if (extension_vif) {
                full_vif |= (vife & 0x7f);
                extension_vif = false;
            } else if (comb_ext) {
                comb_full |= (vife & 0x7f);
                comb_ext = false;
                if (ncomb < WMB_DV_MAX_COMB) combs[ncomb++] = comb_full;
            } else {
                comb_full = vife & 0x7f;
                if (comb_full == 0x7c || comb_full == 0x7f) {
                    comb_full <<= 8;
                    comb_ext = true;
                } else if (ncomb < WMB_DV_MAX_COMB) {
                    combs[ncomb++] = comb_full;
                }
            }
        }
#undef ID_PUSH

        // Build the key.
        char key[WMB_DV_KEY_MAX];
        size_t kl = 0;
        for (size_t i = 0; i < nid && kl + 2 < sizeof(key) - 4; ++i) {
            key[kl++] = HEXU[id_bytes[i] >> 4];
            key[kl++] = HEXU[id_bytes[i] & 0xF];
        }
        key[kl] = 0;
        int count = count_key(s, key, kl) + 1;
        if (count > 1) snprintf(key + kl, sizeof(key) - kl, "_%d", count);

        long remaining = (long)(dend - d);
        if (remaining < 1) break;
        if (variable_length) {
            datalen = *d;
            d++;
            remaining--;
            if (has_difvifs) f = d;
        }
        if (remaining < datalen) datalen = (int)remaining - 1;
        if (datalen < 0) datalen = 0;

        DvEntry* e = new_entry(s);
        if (!e) break;
        strcpy(e->key, key);
        e->offset = (uint16_t)(base_offset + (d - data));
        e->dif = dif;
        e->vif = full_vif;
        e->mtype = mt;
        e->storage = storage;
        e->tariff = (uint16_t)tariff;
        e->subunit = (uint16_t)subunit;
        e->ncomb = (uint8_t)ncomb;
        for (int i = 0; i < ncomb; ++i) e->comb_raw[i] = combs[i];
        e->data = d;
        e->data_len = (uint8_t)datalen;
        s->n++;
        if (ncomb) cp_expand(s, s->n - 1);

        d += datalen;
        if (has_difvifs) f = d;
        if (remaining == datalen || d >= dend) break;
    }
#undef ADV_F
#undef FMT_PUSH
    if (has_difvifs && fmt_len > 0 && !fmt_overflow) {
        s->format_hash = crc16_en13757(fmt_bytes, fmt_len);
        memcpy(s->format_bytes, fmt_bytes, fmt_len);
        s->format_len = (uint8_t)fmt_len;
    }
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool dv_add_synthetic(DvSet* s, const char* dvk, const uint8_t* value, size_t value_len, uint16_t offset) {
    uint8_t b[24];
    size_t n = 0;
    for (const char* p = dvk; p[0] && p[1] && n < sizeof(b); p += 2) {
        int hi = hexv(p[0]), lo = hexv(p[1]);
        if (hi < 0 || lo < 0) return false;
        b[n++] = (uint8_t)(hi << 4 | lo);
    }
    // Same key replaces an earlier entry (upstream map semantics).
    DvEntry* e = nullptr;
    for (int i = 0; i < s->n; ++i) {
        if (strcmp(s->e[i].key, dvk) == 0) { e = &s->e[i]; break; }
    }
    if (!e) {
        e = new_entry(s);
        if (!e) return false;
        s->n++;
    }
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", dvk);
    for (char* k = e->key; *k; ++k) if (*k >= 'a' && *k <= 'f') *k = (char)(*k - 32);
    e->offset = offset;
    e->synthetic = true;
    if (n == 0) return true;
    size_t i = 0;
    e->dif = b[0];
    e->mtype = dif_mtype(b[0]);
    uint32_t storage = (b[0] & 0x40) >> 6;
    int difenr = 0, subunit = 0, tariff = 0;
    while (i < n && (b[i] & 0x80)) {
        i++;
        if (i >= n) break;
        uint8_t dife = b[i];
        subunit |= ((dife & 0x40) >> 6) << difenr;
        tariff |= ((dife & 0x30) >> 4) << (difenr * 2);
        storage |= (uint32_t)(dife & 0x0f) << (1 + difenr * 4);
        difenr++;
    }
    i++;
    e->storage = storage;
    e->tariff = (uint16_t)tariff;
    e->subunit = (uint16_t)subunit;
    if (i < n) {
        uint16_t vif = b[i];
        if ((vif == 0xfb || vif == 0xfd || vif == 0xef || vif == 0xff) && i + 1 < n) vif = (uint16_t)(b[i] << 8 | b[i + 1]);
        e->vif = vif;
    }
    const uint8_t* p = arena_store(s, value, value_len);
    if (!p) return false;
    e->data = p;
    e->data_len = (uint8_t)(value_len > 255 ? 255 : value_len);
    return true;
}

const DvEntry* dv_find(const DvSet* s, const char* key) {
    for (int i = 0; i < s->n; ++i) if (strcmp(s->e[i].key, key) == 0) return &s->e[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Value extraction
// ---------------------------------------------------------------------------
static bool all_ff(const DvEntry* e) {
    for (int i = 0; i < e->data_len; ++i) if (e->data[i] != 0xFF) return false;
    return true;
}

static const int INT_LEN[16] = { 0, 1, 2, 3, 4, 4, 6, 8, 0, 1, 2, 3, 4, -1, 6, 0 };

bool dv_extract_double(const DvEntry* e, double* out, bool auto_scale, bool force_unsigned) {
    int t = e->dif & 0xf;
    if (t == 0x0 || t == 0x8 || t == 0xd || t == 0xf) return false;
    double scale = 1.0;
    if (t == 0x1 || t == 0x2 || t == 0x3 || t == 0x4 || t == 0x6 || t == 0x7) {
        int n = INT_LEN[t];
        if (e->data_len != n) return false;
        uint64_t raw = 0;
        for (int i = n - 1; i >= 0; --i) raw = (raw << 8) | e->data[i];
        double draw = (double)raw;
        if (!force_unsigned) {
            uint64_t sign = (uint64_t)1 << (n * 8 - 1);
            if (raw & sign) {
                uint64_t mask = n == 8 ? 0 : (~(uint64_t)0) << (n * 8);
                draw = (double)(int64_t)(mask | raw);
            }
        }
        if (auto_scale) scale = vif_scale(e->vif);
        *out = draw / scale;
        return true;
    }
    if (t == 0x9 || t == 0xA || t == 0xB || t == 0xC || t == 0xE) {
        int n = INT_LEN[t];
        if (all_ff(e)) { *out = NAN; return false; }
        if (e->data_len != n) return false;
        // The value hex string is in wire order; the most significant byte
        // is last. Its high nibble 'F' marks a negative value.
        uint64_t raw = 0;
        bool negate = false;
        for (int i = n - 1; i >= 0; --i) {
            int hi = e->data[i] >> 4, lo = e->data[i] & 0xF;
            if (i == n - 1 && hi == 0xF) { negate = true; hi = 0; }
            raw = raw * 100 + (uint64_t)(hi * 10 + lo);
        }
        double draw = (double)raw;
        if (negate) draw = -draw;
        if (auto_scale) scale = vif_scale(e->vif);
        *out = draw / scale;
        return true;
    }
    if (t == 0x5) {
        if (e->data_len != 4) return false;
        uint32_t u = (uint32_t)e->data[3] << 24 | (uint32_t)e->data[2] << 16 | (uint32_t)e->data[1] << 8 | e->data[0];
        float fv;
        memcpy(&fv, &u, 4);
        if (auto_scale) scale = vif_scale(e->vif);
        *out = (double)fv / scale;
        return true;
    }
    return false;
}

bool dv_extract_long(const DvEntry* e, uint64_t* out) {
    int t = e->dif & 0xf;
    if (t == 0x1 || t == 0x2 || t == 0x3 || t == 0x4 || t == 0x6 || t == 0x7) {
        int n = INT_LEN[t];
        if (e->data_len != n) return false;
        uint64_t raw = 0;
        for (int i = n - 1; i >= 0; --i) raw = (raw << 8) | e->data[i];
        *out = raw;
        return true;
    }
    if (t == 0x9 || t == 0xA || t == 0xB || t == 0xC || t == 0xE) {
        int n = INT_LEN[t];
        if (all_ff(e)) return false;
        if (e->data_len != n) return false;
        uint64_t raw = 0;
        bool negate = false;
        for (int i = n - 1; i >= 0; --i) {
            int hi = e->data[i] >> 4, lo = e->data[i] & 0xF;
            if (i == n - 1 && hi == 0xF) { negate = true; hi = 0; }
            raw = raw * 100 + (uint64_t)(hi * 10 + lo);
        }
        if (negate) raw = (uint64_t)(-(int64_t)raw);
        *out = raw;
        return true;
    }
    return false;
}

void dv_extract_hex(const DvEntry* e, char* out, size_t out_max) {
    size_t o = 0;
    for (int i = 0; i < e->data_len && o + 2 < out_max; ++i) {
        out[o++] = HEXU[e->data[i] >> 4];
        out[o++] = HEXU[e->data[i] & 0xF];
    }
    if (out_max) out[o] = 0;
}

static bool likely_ascii(const uint8_t* v, int n) {
    int i = 0;
    for (; i < n; ++i) if (v[i] != 0) break;
    if (i == n) return false;
    for (; i < n; ++i) {
        if (v[i] < 20 || v[i] > 126) {
            if (v[i] != 0x0C && v[i] != 0x0A) return false;
        }
    }
    return true;
}

static void safe_append(char* out, size_t out_max, size_t* o, uint8_t ch) {
    if (ch >= 32 && ch < 127 && ch != '<' && ch != '>') {
        if (*o + 1 < out_max) out[(*o)++] = (char)ch;
    } else if (*o + 4 < out_max) {
        out[(*o)++] = '<';
        out[(*o)++] = HEXU[ch >> 4];
        out[(*o)++] = HEXU[ch & 0xF];
        out[(*o)++] = '>';
    }
}

void dv_extract_readable(const DvEntry* e, char* out, size_t out_max, bool reversed_mode) {
    int t = e->dif & 0xf;
    size_t o = 0;
    bool binary = (t == 0x1 || t == 0x2 || t == 0x3 || t == 0x4 || t == 0x6 || t == 0x7 || t == 0xD);
    bool bcd = (t == 0x9 || t == 0xA || t == 0xB || t == 0xC || t == 0xE);
    if (reversed_mode) {
        // "Reversed" readable strings keep the wire order.
        if (binary && likely_ascii(e->data, e->data_len)) {
            for (int i = 0; i < e->data_len; ++i) safe_append(out, out_max, &o, e->data[i]);
        } else {
            dv_extract_hex(e, out, out_max);
            return;
        }
    } else if (binary || bcd) {
        if (binary && likely_ascii(e->data, e->data_len)) {
            for (int i = e->data_len - 1; i >= 0; --i) safe_append(out, out_max, &o, e->data[i]);
        } else {
            for (int i = e->data_len - 1; i >= 0 && o + 2 < out_max; --i) {
                out[o++] = HEXU[e->data[i] >> 4];
                out[o++] = HEXU[e->data[i] & 0xF];
            }
        }
    } else {
        dv_extract_hex(e, out, out_max);
        return;
    }
    if (out_max) out[o < out_max ? o : out_max - 1] = 0;
}

static bool date_g(uint8_t hi, uint8_t lo, DvDate* d) {
    int day = 0x1f & lo;
    int year1 = (0xe0 & lo) >> 5;
    int month = 0x0f & hi;
    int year2 = (0xf0 & hi) >> 1;
    d->year = 2000 + year1 + year2;
    d->month = month;
    d->day = day;
    return month <= 12;
}

static bool time_hm(uint8_t hi, uint8_t lo, DvDate* d) {
    d->minute = 0x3f & lo;
    d->hour = 0x1f & hi;
    return d->minute <= 59 && d->hour <= 23;
}

bool dv_extract_date(const DvEntry* e, DvDate* out) {
    memset(out, 0, sizeof(*out));
    const uint8_t* v = e->data;
    bool ok = true;
    if (e->data_len == 2) {
        ok &= date_g(v[1], v[0], out);
    } else if (e->data_len == 4) {
        ok &= date_g(v[3], v[2], out);
        ok &= time_hm(v[1], v[0], out);
        out->has_time = true;
    } else if (e->data_len == 6) {
        ok &= date_g(v[4], v[3], out);
        ok &= time_hm(v[2], v[1], out);
        out->second = 0x3f & v[0];
        out->has_time = true;
        out->has_seconds = true;
    } else {
        // Upstream leaves the struct zeroed (1900-00-00) and reports success.
        out->year = 1900;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Civil time (Howard Hinnant's algorithms), normalizing like mktime().
// ---------------------------------------------------------------------------
static long days_from_civil(long y, long m, long d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

double civil_to_unix(int y, int mon, int d, int h, int mi, int s) {
    long m0 = (long)mon - 1;  // may be out of range: normalize like mktime
    long yy = y + (m0 >= 0 ? m0 / 12 : -((11 - m0) / 12));
    long mm = m0 - (yy - y) * 12 + 1;
    long days = days_from_civil(yy, mm, 1) + (long)d - 1;
    return (double)days * 86400.0 + (double)h * 3600.0 + (double)mi * 60.0 + (double)s;
}

void unix_to_civil(double t, int* y, int* mon, int* d, int* h, int* mi, int* s) {
    long long secs = (long long)floor(t + 0.5);
    long long z = secs >= 0 ? secs / 86400 : -((86399 - secs) / 86400);
    long long rem = secs - z * 86400;
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long dd = doy - (153 * mp + 2) / 5 + 1;
    long long mm = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (mm <= 2));
    *mon = (int)mm;
    *d = (int)dd;
    *h = (int)(rem / 3600);
    *mi = (int)((rem % 3600) / 60);
    *s = (int)(rem % 60);
}

} // namespace wmb
