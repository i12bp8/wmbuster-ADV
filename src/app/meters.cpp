// wM-Buster ADV — meters seen on air and the live feed.
// GPL-3.0
#include "meters.h"
#include "settings.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

namespace wmb {

static Meter s_meters[MAX_METERS];
static int s_n = 0;
static FeedEntry s_feed[FEED_LEN];
static int s_feed_head = 0;
static int s_feed_n = 0;

static bool skip_default_field(const char* tok, size_t n) {
    static const char* const SKIP[] = { "name", "id", "timestamp", "media", "meter", "driver", "status" };
    for (const char* s : SKIP)
        if (strlen(s) == n && !strncmp(s, tok, n)) return true;
    return false;
}

static const OutField* find_visible(const DecodeResult& r, const char* name, size_t n) {
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& f = r.fields[i];
        if (!f.hidden && strlen(f.name) == n && !strncmp(f.name, name, n)) return &f;
    }
    return nullptr;
}

static bool usable(const OutField* f) {
    if (!f || f->hidden) return false;
    if (f->is_text) return f->text && f->text[0] && strcmp(f->text, "null") != 0;
    return f->value == f->value;  // not NaN
}

void result_summary(const DecodeResult& r, char* out, size_t cap) {
    out[0] = 0;
    const OutField* pick = nullptr;
    if (r.driver && r.driver->default_fields) {
        // The driver's default fields are what wmbusmeters prints by default.
        const char* p = r.driver->default_fields;
        while (*p && !pick) {
            const char* e = strchr(p, ',');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (!skip_default_field(p, n)) {
                const OutField* f = find_visible(r, p, n);
                if (usable(f) && !f->is_text) pick = f;
            }
            p = e ? e + 1 : p + n;
        }
    }
    if (!pick) {
        // Generic decode: prefer totals of the usual quantities.
        static const Quantity PREF[] = { Quantity::Volume, Quantity::Energy, Quantity::HCA, Quantity::Power,
                                         Quantity::Temperature };
        for (Quantity q : PREF) {
            for (int i = 0; i < r.num_fields && !pick; ++i)
                if (!r.fields[i].is_text && r.fields[i].quantity == q && usable(&r.fields[i])) pick = &r.fields[i];
            if (pick) break;
        }
    }
    if (!pick) {
        for (int i = 0; i < r.num_fields && !pick; ++i)
            if (!r.fields[i].is_text && !r.fields[i].is_status && usable(&r.fields[i]) &&
                unit_quantity(r.fields[i].unit) != Quantity::PointInTime)
                pick = &r.fields[i];
    }
    if (pick) field_format(pick, out, cap);
}

void result_status(const DecodeResult& r, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < r.num_fields; ++i) {
        const OutField& f = r.fields[i];
        if (f.is_status && f.is_text && f.text) {
            snprintf(out, cap, "%s", f.text);
            return;
        }
    }
}

static Meter* evict_slot() {
    // Drop the meter heard least recently, preferring meters without a
    // configuration (name/key) so configured meters stay listed.
    Meter* victim = nullptr;
    for (int pass = 0; pass < 2 && !victim; ++pass) {
        for (int i = 0; i < s_n; ++i) {
            Meter* m = &s_meters[i];
            if (pass == 0 && meterconf_find(m->id)) continue;
            if (!victim || (int32_t)(m->last_ms - victim->last_ms) < 0) victim = m;
        }
    }
    return victim;
}

Meter* meters_update(const Frame& f, const DecodeResult& r, int16_t rssi, const double* pos, bool* is_new) {
    *is_new = false;
    if (!r.id[0]) return nullptr;
    Meter* m = meters_find(r.id);
    uint32_t now = millis();
    if (!m) {
        m = s_n < MAX_METERS ? &s_meters[s_n++] : evict_slot();
        if (!m) return nullptr;
        memset(m, 0, sizeof(*m));
        snprintf(m->id, sizeof(m->id), "%s", r.id);
        m->first_ms = now;
        m->rssi_best = -200;
        *is_new = true;
    } else if (m->count > 0) {
        uint32_t gap = now - m->last_ms;
        // Repeated sends (meters often transmit twice) do not count as spacing.
        if (gap > 2000) m->interval_ms = m->interval_ms ? (m->interval_ms * 3 + gap) / 4 : gap;
    }
    memcpy(m->mfct, r.mfct, sizeof(m->mfct));
    m->mfct_code = r.mfct_code;
    m->version = r.version;
    m->type = r.type;
    m->driver = r.driver;
    m->status = r.status;
    m->decrypt = r.decrypt;
    m->mode = f.mode;
    m->rssi = rssi;
    m->rssi_sum += rssi;
    m->count++;
    m->last_ms = now;
    time_t t = time(nullptr);
    m->last_unix = t > 1600000000 ? (uint32_t)t : 0;
    if (rssi >= m->rssi_best) {
        m->rssi_best = rssi;
        if (pos) {
            m->lat = pos[0];
            m->lon = pos[1];
            m->fix = true;
        }
    }
    if (r.status == DecodeStatus::Ok) {
        char s[sizeof(m->summary)];
        result_summary(r, s, sizeof(s));
        if (s[0]) memcpy(m->summary, s, sizeof(s));
        result_status(r, m->status_txt, sizeof(m->status_txt));
        m->alarm = m->status_txt[0] && strcmp(m->status_txt, "OK") != 0 && strcmp(m->status_txt, "null") != 0;
    }
    if (f.len <= sizeof(m->frame)) {
        memcpy(m->frame, f.data, f.len);
        m->frame_len = f.len;
        m->frame_mode = (uint8_t)f.mode;
        m->frame_format = (uint8_t)f.format;
    }
    return m;
}

Meter* meters_find(const char* id) {
    for (int i = 0; i < s_n; ++i)
        if (!strcmp(s_meters[i].id, id)) return &s_meters[i];
    return nullptr;
}

int meters_count() { return s_n; }
Meter* meters_at(int i) { return (i >= 0 && i < s_n) ? &s_meters[i] : nullptr; }
void meters_clear() { s_n = 0; }

static MeterSort s_sort;
static bool s_alarms_first;

static int rank(const Meter& m) {
    const MeterConf* c = meterconf_find(m.id);
    if (c && c->starred) return 0;
    if (s_alarms_first && m.alarm) return 1;
    return 2;
}

static bool before(const Meter& a, const Meter& b) {
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    switch (s_sort) {
    case MeterSort::Signal: if (a.rssi != b.rssi) return a.rssi > b.rssi; break;
    case MeterSort::Id: return strcmp(a.id, b.id) < 0;
    case MeterSort::Count: if (a.count != b.count) return a.count > b.count; break;
    default: break;
    }
    return (int32_t)(a.last_ms - b.last_ms) > 0;
}

int meters_sorted(MeterSort how, bool alarms_first, uint8_t* idx, int cap) {
    s_sort = how;
    s_alarms_first = alarms_first;
    int n = s_n < cap ? s_n : cap;
    for (int i = 0; i < n; ++i) idx[i] = (uint8_t)i;
    for (int i = 1; i < n; ++i) {
        uint8_t v = idx[i];
        int j = i;
        while (j > 0 && before(s_meters[v], s_meters[idx[j - 1]])) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = v;
    }
    return n;
}

void feed_add(const FeedEntry& e) {
    s_feed[s_feed_head] = e;
    s_feed_head = (s_feed_head + 1) % FEED_LEN;
    if (s_feed_n < FEED_LEN) s_feed_n++;
}

int feed_count() { return s_feed_n; }

const FeedEntry* feed_at(int i) {
    if (i < 0 || i >= s_feed_n) return nullptr;
    int k = (s_feed_head - 1 - i + FEED_LEN) % FEED_LEN;
    return &s_feed[k];
}

void feed_clear() {
    s_feed_n = 0;
    s_feed_head = 0;
}

} // namespace wmb
