// wM-Buster ADV — on-device user interface.
// GPL-3.0
#include "ui_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "radio/radio.h"
#include "wmbus/engine.h"
#include "../app/app.h"
#include "../app/meters.h"
#include "../app/sdlog.h"
#include "../app/settings.h"
#include "../hw/board.h"
#include "../hw/gnss.h"
#include "../net/mqtt.h"
#include "../net/net.h"
#include "../version.h"

#ifndef ARDUINO
uint32_t millis();
#endif

namespace wmb {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
struct Theme {
    const char* name;
    uint32_t bg, panel, panel2, line, text, muted, accent, accent2, ok, warn, bad;
};

static const Theme THEMES[] = {
    { "Night", 0x0A0E16, 0x141B29, 0x1C2537, 0x283449, 0xE8EEF8, 0x7D8BA6, 0x38D3FF, 0x9B87FF, 0x3DDC84, 0xFFB020, 0xFF5A5A },
    { "Amber", 0x0E0A05, 0x1C150B, 0x2A2011, 0x3D2F17, 0xFFEBC6, 0xA8906A, 0xFFB23E, 0xFF7A3D, 0x9CD65A, 0xFFC857, 0xFF5A45 },
    { "Ocean", 0x05131A, 0x0B222C, 0x11303D, 0x1A4152, 0xDDF7FF, 0x6DA2B4, 0x2EE6C8, 0x3AA8FF, 0x3DDC97, 0xFFC85A, 0xFF6B6B },
    { "Paper", 0xEEF2F7, 0xFFFFFF, 0xE2E8F0, 0xC6D1DF, 0x111C2E, 0x5C6C84, 0x0068D9, 0x7B4DFF, 0x138A42, 0xC96A00, 0xD12A2A },
};
static const Theme* T = &THEMES[0];

static const int W = 240, H = 135;
static const int BAR_H = 15;       // status bar
static const int HINT_H = 11;      // key hints
static const int CY = BAR_H;       // content top
static const int CH = H - BAR_H - HINT_H;

enum class Media : uint8_t { Water, WarmWater, Heat, Cooling, Electricity, Gas, Hca, Smoke, Room, Other };

static Media media_of(uint8_t type) {
    switch (type) {
    case 0x07: case 0x16: case 0x17: case 0x28: return Media::Water;
    case 0x06: case 0x15: return Media::WarmWater;
    case 0x04: case 0x0C: case 0x0D: return Media::Heat;
    case 0x0A: case 0x0B: return Media::Cooling;
    case 0x02: return Media::Electricity;
    case 0x03: return Media::Gas;
    case 0x08: return Media::Hca;
    case 0x1A: return Media::Smoke;
    case 0x1B: case 0x1C: return Media::Room;
    default: return Media::Other;
    }
}

static uint32_t media_color(Media m) {
    switch (m) {
    case Media::Water: return 0x3B9BFF;
    case Media::WarmWater: return 0xFF6F91;
    case Media::Heat: return 0xFF7A2E;
    case Media::Cooling: return 0x38D3FF;
    case Media::Electricity: return 0xFFC21A;
    case Media::Gas: return 0x7BD84A;
    case Media::Hca: return 0xB07CFF;
    case Media::Smoke: return 0xFF5A5A;
    case Media::Room: return 0x2EE6C8;
    default: return 0x8A96AD;
    }
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r = (int)(ar + (br - ar) * t), g = (int)(ag + (bg - ag) * t), bl = (int)(ab + (bb - ab) * t);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static Gfx* G = nullptr;

static void font(const lgfx::IFont* f) { G->setFont(f); }

static void text(int x, int y, const char* s, uint32_t col, lgfx::textdatum_t d = lgfx::top_left) {
    G->setTextColor(col);
    G->setTextDatum(d);
    G->drawString(s, x, y);
}

static int tw(const char* s) { return G->textWidth(s); }

// Truncate with ".." to fit a width; never grows the string.
static void fit(char* s, int maxw) {
    if (tw(s) <= maxw) return;
    size_t len = strlen(s);
    char tmp[72];
    for (size_t n = len > 2 ? len - 2 : 0;; --n) {
        snprintf(tmp, sizeof(tmp), "%.*s..", (int)n, s);
        if (tw(tmp) <= maxw || n == 0) break;
    }
    memcpy(s, tmp, strlen(tmp) + 1);
}

// Units: m3 -> m³ and C -> °C drawn by hand (the fonts are ASCII only).
static int unit_width(const char* u) {
    if (!u || !*u) return 0;
    if (!strcmp(u, "C") || !strcmp(u, "F")) return tw(u) + 5;
    if (!strncmp(u, "m3", 2)) {
        char rest[16];
        snprintf(rest, sizeof(rest), "m%s", u + 2);
        return tw(rest) + 4;
    }
    return tw(u);
}

// Draws the unit starting at x on the baseline (unit font active).
static void draw_unit(int x, int base, const char* u, uint32_t col) {
    if (!u || !*u) return;
    G->setTextDatum(lgfx::baseline_left);
    G->setTextColor(col);
    int cap = (G->fontHeight() * 2) / 3;
    if (!strcmp(u, "C") || !strcmp(u, "F")) {
        G->drawCircle(x + 2, base - cap + 1, 2, col);
        G->drawString(u, x + 5, base);
        return;
    }
    if (!strncmp(u, "m3", 2)) {
        G->drawString("m", x, base);
        int mw = tw("m");
        const lgfx::IFont* cur = G->getFont();
        G->setFont(&fonts::TomThumb);
        G->setTextDatum(lgfx::baseline_left);
        G->drawString("3", x + mw, base - cap + 4);
        G->setFont(cur);
        if (u[2]) G->drawString(u + 2, x + mw + 4, base);
        return;
    }
    G->drawString(u, x, base);
}

// "123.456 m3" right aligned at xr on a common baseline: number in the big
// font, unit in the small one. Returns the width used.
static int draw_value_r(int xr, int base, const char* v, const lgfx::IFont* big, const lgfx::IFont* small, uint32_t col,
                        uint32_t ucol) {
    char num[40];
    snprintf(num, sizeof(num), "%s", v);
    const char* unit = nullptr;
    char* sp = strrchr(num, ' ');
    if (sp && sp[1] && (sp[1] < '0' || sp[1] > '9')) {
        *sp = 0;
        unit = sp + 1;
    }
    font(small);
    int uw = unit ? unit_width(unit) + 2 : 0;
    font(big);
    int nw = tw(num);
    G->setTextColor(col);
    G->setTextDatum(lgfx::baseline_left);
    G->drawString(num, xr - uw - nw, base);
    if (unit) {
        font(small);
        draw_unit(xr - uw + 2, base, unit, ucol);
    }
    return nw + uw;
}

static void rssi_bars(int x, int y, int rssi, uint32_t dim) {
    int lvl = rssi > -70 ? 4 : rssi > -82 ? 3 : rssi > -94 ? 2 : rssi > -105 ? 1 : 0;
    uint32_t col = lvl >= 3 ? T->ok : lvl == 2 ? T->warn : T->bad;
    for (int i = 0; i < 4; ++i) {
        int h = 3 + i * 2;
        G->fillRect(x + i * 3, y + 9 - h, 2, h, i < lvl ? col : dim);
    }
}

static void age_str(uint32_t ms, char* out, size_t cap) {
    uint32_t s = ms / 1000;
    if (s < 3) snprintf(out, cap, "now");
    else if (s < 60) snprintf(out, cap, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
    else snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

// Media badge: rounded square with a glyph.
static void badge(int x, int y, int sz, Media m, bool dim) {
    uint32_t c = media_color(m);
    if (dim) c = mix(c, T->panel, 0.55f);
    uint32_t fg = 0xFFFFFF;
    G->fillSmoothRoundRect(x, y, sz, sz, 5, c);
    int cx = x + sz / 2, cy = y + sz / 2;
    switch (m) {
    case Media::Water:
    case Media::WarmWater:
    case Media::Cooling:
        G->fillSmoothCircle(cx, cy + 3, 5, fg);
        G->fillTriangle(cx - 5, cy + 2, cx + 5, cy + 2, cx, cy - 7, fg);
        break;
    case Media::Heat:
    case Media::Gas:
        G->fillSmoothCircle(cx, cy + 3, 5, fg);
        G->fillTriangle(cx - 5, cy + 2, cx + 5, cy + 2, cx + 1, cy - 8, fg);
        G->fillTriangle(cx - 5, cy + 1, cx - 1, cy + 1, cx - 4, cy - 4, fg);
        G->fillSmoothCircle(cx, cy + 4, 2, c);
        break;
    case Media::Electricity:
        G->fillTriangle(cx + 2, cy - 8, cx - 5, cy + 1, cx, cy + 1, fg);
        G->fillTriangle(cx - 2, cy + 8, cx + 5, cy - 1, cx, cy - 1, fg);
        break;
    case Media::Hca:
        for (int i = -1; i <= 1; ++i) G->fillSmoothRoundRect(cx + i * 5 - 2, cy - 6, 4, 12, 2, fg);
        G->fillRect(cx - 7, cy + 4, 14, 2, fg);
        break;
    case Media::Smoke:
        G->drawCircle(cx, cy, 7, fg);
        G->drawCircle(cx, cy, 4, fg);
        G->fillCircle(cx, cy, 1, fg);
        break;
    case Media::Room:
        G->fillSmoothRoundRect(cx - 2, cy - 7, 5, 11, 2, fg);
        G->fillSmoothCircle(cx, cy + 4, 4, fg);
        G->fillSmoothCircle(cx, cy + 4, 2, c);
        break;
    default:
        font(&fonts::FreeSansBold9pt7b);
        text(cx + 1, cy + 1, "?", fg, lgfx::middle_center);
        break;
    }
}

static void lock_icon(int x, int y, uint32_t c) {
    G->drawRoundRect(x + 2, y, 5, 6, 2, c);
    G->fillRoundRect(x, y + 4, 9, 6, 1, c);
}

static void star_icon(int x, int y, int r, uint32_t c) {
    float pts[10][2];
    for (int i = 0; i < 10; ++i) {
        float a = -1.5708f + i * 0.6283f;
        float rr = (i & 1) ? r * 0.45f : r;
        pts[i][0] = x + cosf(a) * rr;
        pts[i][1] = y + sinf(a) * rr;
    }
    for (int i = 0; i < 10; i += 2) {
        int j = (i + 2) % 10;
        G->fillTriangle(x, y, (int)pts[i][0], (int)pts[i][1], (int)pts[(i + 1) % 10][0], (int)pts[(i + 1) % 10][1], c);
        G->fillTriangle(x, y, (int)pts[(i + 1) % 10][0], (int)pts[(i + 1) % 10][1], (int)pts[j][0], (int)pts[j][1], c);
    }
}

static void wifi_icon(int x, int y, uint32_t c, bool on) {
    uint32_t col = on ? c : T->line;
    G->fillArc(x + 5, y + 9, 8, 7, 225, 315, col);
    G->fillArc(x + 5, y + 9, 5, 4, 225, 315, col);
    G->fillCircle(x + 5, y + 8, 1, col);
}

static void battery_icon(int x, int y, int pct, bool charging) {
    uint32_t c = pct < 0 ? T->muted : pct < 15 ? T->bad : pct < 35 ? T->warn : T->ok;
    G->drawRoundRect(x, y, 17, 9, 2, T->muted);
    G->fillRect(x + 17, y + 3, 2, 3, T->muted);
    if (pct >= 0) G->fillRect(x + 2, y + 2, (13 * (pct > 100 ? 100 : pct)) / 100, 5, c);
    if (charging) {
        G->fillTriangle(x + 9, y + 1, x + 5, y + 5, x + 8, y + 5, T->text);
        G->fillTriangle(x + 8, y + 8, x + 12, y + 4, x + 9, y + 4, T->text);
    }
}

static void sparkline(int x, int y, int w, int h, const int16_t* v, int n, int lo, int hi, uint32_t col, bool fill) {
    if (n < 2) return;
    float sx = (float)(w - 1) / (float)(n - 1);
    int px = 0, py = 0;
    for (int i = 0; i < n; ++i) {
        int vv = v[i] < lo ? lo : v[i] > hi ? hi : v[i];
        int yy = y + h - 1 - (int)((float)(vv - lo) * (h - 1) / (float)(hi - lo));
        int xx = x + (int)(i * sx);
        if (fill) G->drawFastVLine(xx, yy, y + h - yy, mix(col, T->panel, 0.7f));
        if (i) G->drawLine(px, py, xx, yy, col);
        px = xx;
        py = yy;
    }
}

static const char* mfct_or(const Meter* m) { return m->mfct[0] ? m->mfct : "???"; }

static void meter_title(const Meter* m, char* out, size_t cap) {
    const MeterConf* c = meterconf_find(m->id);
    if (c && c->name[0]) snprintf(out, cap, "%s", c->name);
    else snprintf(out, cap, "%s", m->id);
}

static void meter_subtitle(const Meter* m, char* out, size_t cap) {
    const MeterConf* c = meterconf_find(m->id);
    const char* what = m->driver ? m->driver->name : media_name(m->type, m->mfct_code);
    if (c && c->name[0]) snprintf(out, cap, "%s %s %s", m->id, mfct_or(m), what);
    else snprintf(out, cap, "%s  %s", mfct_or(m), what);
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
static const char* const TAB_NAMES[] = { "METERS", "LIVE", "HUNT", "STATS", "SETUP" };

void UiView::begin(Gfx* g) {
    g_ = g;
    G = g;
    set_theme(g_cfg.theme);
    memset(hist_, 0, sizeof(hist_));
    memset(rate_, 0, sizeof(rate_));
}

void UiView::set_theme(uint8_t idx) {
    T = &THEMES[idx < sizeof(THEMES) / sizeof(THEMES[0]) ? idx : 0];
    dirty_ = true;
}

void UiView::set_screen(Screen s) {
    screen_ = s;
    dirty_ = true;
}

void UiView::open_meter(const char* id) {
    snprintf(detail_id_, sizeof(detail_id_), "%s", id);
    detail_scroll_ = 0;
    if (screen_ != Screen::Detail) back_ = screen_ < Screen::TabCount ? screen_ : Screen::Meters;
    screen_ = Screen::Detail;
    dirty_ = true;
}

void UiView::track(const char* id) {
    if (strcmp(track_id_, id) != 0) {
        hist_n_ = 0;
        track_seen_ = 0;
    }
    snprintf(track_id_, sizeof(track_id_), "%s", id);
    screen_ = Screen::Hunt;
    dirty_ = true;
}

void UiView::toast(const char* msg, uint32_t color, uint32_t now) {
    snprintf(toast_, sizeof(toast_), "%s", msg);
    toast_color_ = color;
    toast_until_ = now + 2600;
    dirty_ = true;
}

void UiView::splash(const char* status, uint32_t now) {
    G = g_;
    g_->fillScreen(T->bg);
    // Concentric "radio waves" behind the logo.
    for (int i = 0; i < 4; ++i) {
        uint32_t c = mix(T->accent, T->bg, 0.55f + i * 0.12f);
        g_->drawArc(38, 58, 14 + i * 9, 13 + i * 9, 300, 60, c);
    }
    g_->fillSmoothCircle(38, 58, 6, T->accent);
    font(&fonts::FreeSansBold12pt7b);
    text(62, 36, "wM-Buster", T->text);
    font(&fonts::FreeSansBold9pt7b);
    int w = tw("ADV");
    g_->fillSmoothRoundRect(62, 64, w + 12, 18, 5, T->accent);
    text(68, 66, "ADV", T->bg);
    font(&fonts::DejaVu9);
    text(62 + w + 18, 69, "wireless M-Bus", T->muted);
    text(62, 90, "C1  T1  S1  |  " WMB_VERSION, T->muted);
    font(&fonts::DejaVu9);
    g_->fillRect(0, H - 16, W, 16, T->panel);
    text(W / 2, H - 13, status, T->text, lgfx::top_center);
    (void)now;
}

// ---------------------------------------------------------------------------
// Status bar and hints
// ---------------------------------------------------------------------------
static void tab_icon(int i, int x, int y, uint32_t c) {
    switch (i) {
    case 0:  // meters: list
        for (int k = 0; k < 3; ++k) {
            G->fillRect(x, y + 1 + k * 3, 2, 2, c);
            G->fillRect(x + 3, y + 1 + k * 3, 6, 2, c);
        }
        break;
    case 1:  // live: pulse
        G->drawLine(x, y + 5, x + 2, y + 5, c);
        G->drawLine(x + 2, y + 5, x + 4, y + 1, c);
        G->drawLine(x + 4, y + 1, x + 6, y + 8, c);
        G->drawLine(x + 6, y + 8, x + 7, y + 5, c);
        G->drawLine(x + 7, y + 5, x + 9, y + 5, c);
        break;
    case 2:  // hunt: target
        G->drawCircle(x + 4, y + 4, 4, c);
        G->fillCircle(x + 4, y + 4, 1, c);
        break;
    case 3:  // stats: bars
        G->fillRect(x, y + 5, 2, 4, c);
        G->fillRect(x + 3, y + 2, 2, 7, c);
        G->fillRect(x + 6, y + 4, 2, 5, c);
        break;
    default:  // setup: gear
        G->fillCircle(x + 4, y + 4, 3, c);
        G->fillRect(x + 3, y, 3, 9, c);
        G->fillRect(x, y + 3, 9, 3, c);
        G->fillCircle(x + 4, y + 4, 1, T->panel);
        break;
    }
}

void UiView::draw_status_bar(uint32_t now) {
    g_->fillRect(0, 0, W, BAR_H, T->panel);
    g_->drawFastHLine(0, BAR_H - 1, W, T->line);
    font(&fonts::Font0);
    int x = 3;
    Screen tab = screen_ < Screen::TabCount ? screen_ : back_;
    for (int i = 0; i < (int)Screen::TabCount; ++i) {
        if ((int)tab == i) {
            int w = 14 + tw(TAB_NAMES[i]) + 5;
            g_->fillSmoothRoundRect(x, 2, w, 11, 5, T->accent);
            tab_icon(i, x + 3, 3, T->bg);
            text(x + 14, 4, TAB_NAMES[i], T->bg);
            x += w + 4;
        } else {
            tab_icon(i, x + 1, 3, T->muted);
            x += 14;
        }
    }
    // Right side status icons, from the right edge.
    int xr = W - 3;
    int pct = board_battery_pct();
    battery_icon(xr - 19, 3, pct, board_charging());
    xr -= 23;
    NetStatus ns;
    net_status(&ns);
    if (sdlog_card_ok()) {
        g_->fillRect(xr - 6, 3, 6, 9, T->muted);
        g_->fillTriangle(xr - 6, 3, xr - 6, 5, xr - 4, 3, T->panel);
        xr -= 9;
    }
    GnssInfo gi;
    gnss_info(&gi);
    if (gi.present) {
        uint32_t c = gi.fix ? T->ok : T->warn;
        g_->drawCircle(xr - 4, 7, 4, c);
        g_->fillCircle(xr - 4, 7, 1, c);
        xr -= 11;
    }
    if (g_cfg.mqtt_enabled) {
        font(&fonts::Font0);
        text(xr - 5, 4, "M", ns.mqtt_connected ? T->ok : T->bad);
        xr -= 9;
    }
    bool wifi_on = ns.ap_on || ns.sta_connected;
    if (g_cfg.wifi_mode) {
        wifi_icon(xr - 11, 2, ns.sta_connected ? T->ok : T->accent, wifi_on);
        xr -= 13;
    }
    // RX activity dot.
    bool flash = now - rx_flash_ < 250;
    g_->fillSmoothCircle(xr - 4, 7, 3, flash ? T->ok : (radio_ready() ? T->line : T->bad));
}

void UiView::draw_hints(const char* hint) {
    int y = H - HINT_H;
    g_->fillRect(0, y, W, HINT_H, T->bg);
    g_->drawFastHLine(0, y, W, T->line);
    font(&fonts::Font0);
    char r[32];
    snprintf(r, sizeof(r), "%s %lu/m", radio_ready() ? radio_band_name(radio_band()) : "NO RADIO",
             (unsigned long)g_app.per_min);
    int rw = tw(r);
    text(W - 3, y + 2, r, radio_ready() ? T->muted : T->bad, lgfx::top_right);
    char h[64];
    snprintf(h, sizeof(h), "%s", hint);
    fit(h, W - rw - 12);
    text(3, y + 2, h, T->muted);
}

void UiView::draw_toast(uint32_t now) {
    if (!toast_[0] || (int32_t)(now - toast_until_) > 0) {
        toast_[0] = 0;
        return;
    }
    font(&fonts::DejaVu9);
    int w = tw(toast_) + 20;
    if (w > W - 8) w = W - 8;
    int x = (W - w) / 2, y = H - HINT_H - 20;
    g_->fillSmoothRoundRect(x, y, w, 17, 6, T->panel2);
    g_->drawRoundRect(x, y, w, 17, 6, toast_color_);
    g_->fillSmoothCircle(x + 8, y + 8, 3, toast_color_);
    char t[48];
    snprintf(t, sizeof(t), "%s", toast_);
    fit(t, w - 20);
    text(x + 15, y + 4, t, T->text);
}

void UiView::draw_empty_radar(uint32_t now, int y0, int h) {
    int cx = W / 2, cy = y0 + h / 2 - 6;
    for (int i = 1; i <= 3; ++i) g_->drawCircle(cx, cy, i * 13, T->line);
    float a = (float)(now % 2400) / 2400.0f * 6.2832f;
    for (int k = 0; k < 14; ++k) {
        float aa = a - k * 0.06f;
        uint32_t c = mix(T->accent, T->bg, (float)k / 14.0f);
        g_->drawLine(cx, cy, cx + (int)(cosf(aa) * 38), cy + (int)(sinf(aa) * 38), c);
    }
    g_->fillSmoothCircle(cx, cy, 3, T->accent);
    font(&fonts::DejaVu9);
    char s[48];
    snprintf(s, sizeof(s), radio_ready() ? "Listening on %s" : "No radio detected", app_band_label());
    text(cx, cy + 44, s, radio_ready() ? T->muted : T->bad, lgfx::top_center);
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------
static const int ROW_H = 27;
static uint8_t s_idx[MAX_METERS];

void UiView::draw_meters(uint32_t now) {
    int n = meters_sorted((MeterSort)sort_, true, s_idx, MAX_METERS);
    if (n == 0) {
        draw_empty_radar(now, CY, CH);
        draw_hints("TAB/1-5 views  ;. scroll");
        return;
    }
    if (sel_ >= n) sel_ = n - 1;
    if (sel_ < 0) sel_ = 0;
    int vis = CH / ROW_H;
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + vis) top_ = sel_ - vis + 1;
    float target = (float)((sel_ - top_) * ROW_H);
    sel_y_ += (target - sel_y_) * 0.45f;
    if (fabsf(target - sel_y_) < 0.6f) sel_y_ = target;
    else dirty_ = true;
    int hy = CY + 1 + (int)sel_y_;
    g_->fillSmoothRoundRect(2, hy, W - 8, ROW_H - 2, 6, T->panel2);
    g_->fillSmoothRoundRect(2, hy + 5, 3, ROW_H - 12, 1, T->accent);

    for (int r = 0; r < vis && top_ + r < n; ++r) {
        const Meter* m = meters_at(s_idx[top_ + r]);
        const MeterConf* c = meterconf_find(m->id);
        int y = CY + 1 + r * ROW_H;
        bool enc = m->status == DecodeStatus::Encrypted;
        bool ok = m->status == DecodeStatus::Ok;
        badge(9, y + 3, 19, media_of(m->type), !ok);
        char t[40], sub[48], val[32], age[8];
        meter_title(m, t, sizeof(t));
        meter_subtitle(m, sub, sizeof(sub));
        // Value / state on the right.
        int vw;
        if (ok && m->summary[0]) {
            vw = draw_value_r(W - 9, y + 15, m->summary, &fonts::FreeSansBold9pt7b, &fonts::DejaVu9, T->text,
                              T->muted);
        } else {
            font(&fonts::DejaVu9);
            const char* st = enc ? (m->decrypt == DecryptStatus::WrongKey ? "wrong key" : "encrypted")
                                 : ok ? "no value" : "undecoded";
            snprintf(val, sizeof(val), "%s", st);
            vw = tw(val) + (enc ? 13 : 0);
            text(W - 9, y + 5, val, enc ? T->warn : T->muted, lgfx::top_right);
            if (enc) lock_icon(W - 9 - vw, y + 4, T->warn);
        }
        // Title.
        int tx = 33;
        if (c && c->starred) {
            star_icon(tx + 4, y + 8, 5, T->warn);
            tx += 11;
        }
        font(&fonts::FreeSansBold9pt7b);
        fit(t, W - 12 - vw - tx - 6);
        text(tx, y + 2, t, T->text);
        // Subtitle + signal + age.
        font(&fonts::DejaVu9);
        age_str(now - m->last_ms, age, sizeof(age));
        int aw = tw(age);
        text(W - 9, y + 17, age, T->muted, lgfx::top_right);
        rssi_bars(W - 9 - aw - 16, y + 16, m->rssi, T->line);
        if (m->alarm) {
            snprintf(sub, sizeof(sub), "%s", m->status_txt);
            for (char* p = sub; *p; ++p) if (*p == '_') *p = ' ';
            fit(sub, W - 9 - aw - 20 - 33);
            text(33, y + 17, sub, T->bad);
        } else {
            fit(sub, W - 9 - aw - 20 - 33);
            text(33, y + 17, sub, T->muted);
        }
    }
    // Scrollbar
    if (n > vis) {
        int th = CH * vis / n;
        if (th < 8) th = 8;
        int ty = CY + (CH - th) * top_ / (n - vis);
        g_->fillRoundRect(W - 3, ty, 2, th, 1, T->line);
    }
    static const char* const SORTS[] = { "recent", "signal", "id", "count" };
    char hint[64];
    snprintf(hint, sizeof(hint), "ENT open T hunt O:%s", SORTS[sort_ & 3]);
    draw_hints(hint);
}

void UiView::input_meters(const UiInput& in) {
    int n = meters_count();
    switch (in.key) {
    case UiKey::Up: if (sel_ > 0) sel_--; break;
    case UiKey::Down: if (sel_ < n - 1) sel_++; break;
    case UiKey::Enter:
        if (n) {
            meters_sorted((MeterSort)sort_, true, s_idx, MAX_METERS);
            open_meter(meters_at(s_idx[sel_])->id);
        }
        break;
    case UiKey::Char:
        if ((in.ch == 't' || in.ch == 'T') && n) {
            meters_sorted((MeterSort)sort_, true, s_idx, MAX_METERS);
            track(meters_at(s_idx[sel_])->id);
        } else if (in.ch == 'o' || in.ch == 'O') {
            sort_ = (uint8_t)((sort_ + 1) % 4);
            sel_ = top_ = 0;
        } else if ((in.ch == 'f' || in.ch == 'F' || in.ch == '*') && n) {
            meters_sorted((MeterSort)sort_, true, s_idx, MAX_METERS);
            const Meter* m = meters_at(s_idx[sel_]);
            const MeterConf* c = meterconf_find(m->id);
            MeterConf mc;
            if (c) mc = *c;
            else {
                memset(&mc, 0, sizeof(mc));
                snprintf(mc.id, sizeof(mc.id), "%s", m->id);
            }
            mc.starred = !mc.starred;
            meterconf_put(mc);
        }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Live feed
// ---------------------------------------------------------------------------
void UiView::draw_live(uint32_t now) {
    int n = feed_count();
    if (n == 0) {
        draw_empty_radar(now, CY, CH);
        draw_hints("Telegrams appear here as they arrive");
        return;
    }
    const int rh = 13;
    int vis = CH / rh;
    if (live_top_ > n - vis) live_top_ = n - vis > 0 ? n - vis : 0;
    for (int r = 0; r < vis && live_top_ + r < n; ++r) {
        const FeedEntry* e = feed_at(live_top_ + r);
        int y = CY + 1 + r * rh;
        uint32_t age_ms = now - e->ms;
        if (age_ms < 1200 && live_top_ == 0) {
            float t = age_ms / 1200.0f;
            g_->fillRect(0, y, W, rh, mix(mix(T->accent, T->panel2, 0.6f), T->bg, t));
        } else if (r & 1) {
            g_->fillRect(0, y, W, rh, T->panel);
        }
        uint32_t sc = e->status == DecodeStatus::Ok ? T->ok : e->status == DecodeStatus::Encrypted ? T->warn : T->muted;
        g_->fillRect(0, y + 2, 3, rh - 4, sc);
        font(&fonts::Font0);
        char age[8];
        age_str(age_ms, age, sizeof(age));
        text(6, y + 3, age, T->muted);
        uint32_t mc = e->mode == LinkMode::C1 ? T->accent2 : e->mode == LinkMode::S1 ? T->warn : T->accent;
        text(30, y + 3, link_mode_name(e->mode), mc);
        text(46, y + 3, e->id, T->text);
        text(98, y + 3, e->mfct, T->muted);
        char v[32];
        if (e->status == DecodeStatus::Ok && e->summary[0]) {
            draw_value_r(W - 30, y + 10, e->summary, &fonts::DejaVu9, &fonts::DejaVu9, T->text, T->muted);
        } else {
            snprintf(v, sizeof(v), "%s", e->status == DecodeStatus::Encrypted ? "encrypted" : decode_status_name(e->status));
            font(&fonts::DejaVu9);
            fit(v, 90);
            text(W - 30, y + 2, v, T->muted, lgfx::top_right);
        }
        char rs[8];
        snprintf(rs, sizeof(rs), "%d", e->rssi);
        font(&fonts::Font0);
        text(W - 3, y + 3, rs, e->rssi > -80 ? T->ok : e->rssi > -95 ? T->warn : T->bad, lgfx::top_right);
    }
    if (live_top_ > 0) dirty_ = true;
    draw_hints(live_top_ ? "paused  ; to top" : ";. scroll  ENT open");
}

void UiView::input_live(const UiInput& in) {
    int n = feed_count();
    if (in.key == UiKey::Up && live_top_ > 0) live_top_--;
    else if (in.key == UiKey::Down && live_top_ < n - 1) live_top_++;
    else if (in.key == UiKey::Enter && n) open_meter(feed_at(live_top_)->id);
}

// ---------------------------------------------------------------------------
// Hunt: follow one meter's signal to find it
// ---------------------------------------------------------------------------
void UiView::draw_hunt(uint32_t now) {
    const Meter* m = track_id_[0] ? meters_find(track_id_) : nullptr;
    if (!m) {
        // Default to the strongest meter heard in the last minute.
        int n = meters_sorted(MeterSort::Signal, false, s_idx, MAX_METERS);
        for (int i = 0; i < n; ++i) {
            const Meter* c = meters_at(s_idx[i]);
            if (now - c->last_ms < 60000) {
                track(c->id);
                screen_ = Screen::Hunt;
                m = c;
                break;
            }
        }
    }
    if (!m) {
        font(&fonts::DejaVu12);
        text(W / 2, CY + 34, "Nothing to hunt yet", T->text, lgfx::top_center);
        font(&fonts::DejaVu9);
        text(W / 2, CY + 54, "Select a meter and press T", T->muted, lgfx::top_center);
        draw_hints("T on a meter starts hunting");
        return;
    }
    char t[40], sub[48];
    meter_title(m, t, sizeof(t));
    badge(6, CY + 5, 19, media_of(m->type), m->status != DecodeStatus::Ok);
    font(&fonts::FreeSansBold9pt7b);
    fit(t, 130);
    text(30, CY + 4, t, T->text);
    meter_subtitle(m, sub, sizeof(sub));
    font(&fonts::DejaVu9);
    fit(sub, 130);
    text(30, CY + 18, sub, T->muted);

    // Big RSSI and gauge.
    int rssi = m->rssi;
    uint32_t col = rssi > -75 ? T->ok : rssi > -92 ? T->warn : T->bad;
    char big[8];
    snprintf(big, sizeof(big), "%d", rssi);
    font(&fonts::DejaVu24);
    text(W - 30, CY + 2, big, col, lgfx::top_right);
    font(&fonts::DejaVu9);
    text(W - 6, CY + 13, "dBm", T->muted, lgfx::top_right);
    int gx = 6, gy = CY + 33, gw = W - 12, gh = 9;
    g_->fillSmoothRoundRect(gx, gy, gw, gh, 4, T->panel2);
    float f = (rssi + 120) / 80.0f;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    int fw = (int)(gw * f);
    for (int x = 0; x < fw; ++x) {
        float p = (float)x / gw;
        uint32_t c = p < 0.4f ? mix(T->bad, T->warn, p / 0.4f) : mix(T->warn, T->ok, (p - 0.4f) / 0.6f);
        g_->drawFastVLine(gx + x, gy + 1, gh - 2, c);
    }
    // Best marker.
    int bx = gx + (int)(gw * ((m->rssi_best + 120) / 80.0f < 0 ? 0 : (m->rssi_best + 120) / 80.0f > 1 ? 1 : (m->rssi_best + 120) / 80.0f));
    g_->drawFastVLine(bx, gy - 2, gh + 4, T->text);

    // History graph.
    int hx = 6, hy = CY + 47, hw = W - 12, hh = CH - 49;
    g_->drawRoundRect(hx, hy, hw, hh, 4, T->line);
    for (int db = -110; db <= -50; db += 20) {
        int yy = hy + hh - 2 - (int)((db + 120) * (hh - 4) / 80.0f);
        for (int x = hx + 3; x < hx + hw - 3; x += 4) g_->drawPixel(x, yy, T->line);
    }
    if (hist_n_ >= 2) sparkline(hx + 2, hy + 2, hw - 4, hh - 4, hist_, hist_n_, -120, -40, T->accent, true);
    char info[48], age[8];
    age_str(now - m->last_ms, age, sizeof(age));
    snprintf(info, sizeof(info), "best %d  last %s  #%lu", m->rssi_best, age, (unsigned long)m->count);
    font(&fonts::Font0);
    text(hx + 5, hy + 4, info, T->muted);
    draw_hints(hunt_beep_ ? "B beep:on  ENT details" : "B beep:off  ENT details");
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
static void tile(int x, int y, int w, int h, const char* label, const char* value, uint32_t vc) {
    G->fillSmoothRoundRect(x, y, w, h, 5, T->panel);
    font(&fonts::Font0);
    text(x + 5, y + 4, label, T->muted);
    font(&fonts::FreeSansBold9pt7b);
    char v[24];
    snprintf(v, sizeof(v), "%s", value);
    fit(v, w - 8);
    text(x + 5, y + 13, v, vc);
}

void UiView::draw_stats(uint32_t now) {
    RadioStats rs;
    radio_get_stats(&rs);
    char v[24];
    int tw4 = (W - 10) / 4, th = 32;
    snprintf(v, sizeof(v), "%lu", (unsigned long)g_app.frames);
    tile(2, CY + 2, tw4 - 2, th, "FRAMES", v, T->text);
    snprintf(v, sizeof(v), "%lu", (unsigned long)g_app.decoded);
    tile(2 + tw4, CY + 2, tw4 - 2, th, "DECODED", v, T->ok);
    snprintf(v, sizeof(v), "%lu", (unsigned long)g_app.encrypted);
    tile(2 + 2 * tw4, CY + 2, tw4 - 2, th, "LOCKED", v, T->warn);
    snprintf(v, sizeof(v), "%d", meters_count());
    tile(2 + 3 * tw4, CY + 2, tw4 - 2, th, "METERS", v, T->accent);

    // Rate graph (telegrams per minute).
    int gx = 2, gy = CY + 37, gw = 130, gh = CH - 39;
    g_->fillSmoothRoundRect(gx, gy, gw, gh, 5, T->panel);
    font(&fonts::Font0);
    char lab[32];
    snprintf(lab, sizeof(lab), "RATE %lu/min", (unsigned long)g_app.per_min);
    text(gx + 5, gy + 4, lab, T->muted);
    int16_t vals[48];
    int maxv = 4;
    for (int i = 0; i < rate_n_; ++i) {
        vals[i] = (int16_t)rate_[i];
        if (vals[i] > maxv) maxv = vals[i];
    }
    if (rate_n_ >= 2) sparkline(gx + 4, gy + 14, gw - 8, gh - 18, vals, rate_n_, 0, maxv, T->accent, true);

    // Radio / link details.
    int lx = 136, ly = gy;
    g_->fillSmoothRoundRect(lx, ly, W - lx - 2, gh, 5, T->panel);
    font(&fonts::Font0);
    char line[40];
    int yy = ly + 4;
    auto row = [&](const char* k, const char* val, uint32_t c) {
        text(lx + 5, yy, k, T->muted);
        text(W - 7, yy, val, c, lgfx::top_right);
        yy += 10;
    };
    row("radio", radio_chip_name(), radio_ready() ? T->text : T->bad);
    row("band", app_band_label(), T->text);
    snprintf(line, sizeof(line), "%d dBm", rs.noise_floor);
    row("floor", line, T->text);
    snprintf(line, sizeof(line), "%lu", (unsigned long)g_app.crc_errors);
    row("crc err", line, g_app.crc_errors ? T->warn : T->text);
    NetStatus ns;
    net_status(&ns);
    row("ip", ns.sta_connected ? ns.sta_ip : ns.ap_on ? ns.ap_ip : "off", T->text);
    uint32_t up = now / 1000;
    snprintf(line, sizeof(line), "%luh%02lum", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
    row("uptime", line, T->text);
    draw_hints("more in the web UI");
}

// ---------------------------------------------------------------------------
// Detail
// ---------------------------------------------------------------------------
void UiView::draw_detail(uint32_t now) {
    Meter* m = meters_find(detail_id_);
    if (!m) {
        screen_ = back_;
        dirty_ = true;
        return;
    }
    const MeterConf* c = meterconf_find(m->id);
    const Decoder* d = app_redecode(m);
    char t[40], sub[64], age[8];
    meter_title(m, t, sizeof(t));
    badge(4, CY + 4, 19, media_of(m->type), m->status != DecodeStatus::Ok);
    font(&fonts::FreeSansBold9pt7b);
    int tx = 28;
    if (c && c->starred) {
        star_icon(tx + 4, CY + 10, 5, T->warn);
        tx += 11;
    }
    fit(t, 150 - tx);
    text(tx, CY + 2, t, T->text);
    const char* mn = manufacturer_name(m->mfct_code);
    if (c && c->name[0])
        snprintf(sub, sizeof(sub), "%s %s  %s  v%02X t%02X", m->id, mfct_or(m), m->driver ? m->driver->name : "-",
                 m->version, m->type);
    else
        snprintf(sub, sizeof(sub), "%s %s  %s  v%02X t%02X", mfct_or(m), mn ? mn : "", m->driver ? m->driver->name : "-",
                 m->version, m->type);
    font(&fonts::Font0);
    fit(sub, W - 30);
    text(28, CY + 20, sub, T->muted);
    age_str(now - m->last_ms, age, sizeof(age));
    char rs[24];
    snprintf(rs, sizeof(rs), "%d dBm  %s", m->rssi, age);
    text(W - 4, CY + 4, rs, T->muted, lgfx::top_right);

    int y0 = CY + 32;
    g_->drawFastHLine(0, y0 - 3, W, T->line);
    const int rh = 12;
    int vis = (H - HINT_H - y0) / rh;
    int shown = 0;
    if (d && d->res.status == DecodeStatus::Ok) {
        const DecodeResult& r = d->res;
        int rows = 0;
        for (int i = 0; i < r.num_fields; ++i) if (!r.fields[i].hidden) rows++;
        if (detail_scroll_ > rows - vis) detail_scroll_ = rows - vis > 0 ? rows - vis : 0;
        int idx = 0;
        for (int i = 0; i < r.num_fields && shown < vis; ++i) {
            const OutField& f = r.fields[i];
            if (f.hidden) continue;
            if (idx++ < detail_scroll_) continue;
            int y = y0 + shown * rh;
            if (shown & 1) g_->fillRect(0, y - 1, W, rh, T->panel);
            char name[40], val[48];
            snprintf(name, sizeof(name), "%s", f.vname);
            for (char* p = name; *p; ++p) if (*p == '_') *p = ' ';
            field_format(&f, val, sizeof(val));
            font(&fonts::DejaVu9);
            fit(name, 110);
            text(4, y, name, T->muted);
            bool bad = f.is_status && strcmp(f.text, "OK") != 0 && strcmp(f.text, "null") != 0;
            if (f.is_text) {
                fit(val, W - 120);
                text(W - 4, y, val, bad ? T->bad : T->text, lgfx::top_right);
            } else {
                draw_value_r(W - 4, y + 8, val, &fonts::DejaVu9, &fonts::DejaVu9, T->text, T->muted);
            }
            shown++;
        }
        if (rows > vis) {
            int th = (H - HINT_H - y0) * vis / rows;
            int ty = y0 + (H - HINT_H - y0 - th) * detail_scroll_ / (rows - vis);
            g_->fillRoundRect(W - 2, ty, 2, th, 1, T->line);
        }
    } else {
        font(&fonts::DejaVu12);
        bool enc = m->status == DecodeStatus::Encrypted;
        const char* msg = enc ? (m->decrypt == DecryptStatus::WrongKey ? "Wrong key" : "Encrypted telegram")
                              : m->status == DecodeStatus::CompactUnknown ? "Compact frame, format not seen yet"
                              : m->status == DecodeStatus::MfctPayload ? "Manufacturer specific payload"
                                                                       : "No values";
        if (enc) lock_icon(W / 2 - 4, y0 + 8, T->warn);
        text(W / 2, y0 + 22, msg, enc ? T->warn : T->text, lgfx::top_center);
        font(&fonts::DejaVu9);
        text(W / 2, y0 + 40, enc ? "Press K to enter the AES key" : "Press R to see the raw telegram", T->muted,
             lgfx::top_center);
    }
    draw_hints("K key N name F star T R");
}

void UiView::input_detail(const UiInput& in, uint32_t now) {
    Meter* m = meters_find(detail_id_);
    if (!m) return;
    if (in.key == UiKey::Up && detail_scroll_ > 0) detail_scroll_--;
    else if (in.key == UiKey::Down) detail_scroll_++;
    else if (in.key == UiKey::Back || in.key == UiKey::Esc || in.key == UiKey::Left) screen_ = back_;
    else if (in.key == UiKey::Char) {
        char ch = (char)(in.ch | 0x20);
        const MeterConf* c = meterconf_find(m->id);
        if (ch == 'k') {
            edit_ = true;
            edit_kind_ = 1;
            snprintf(edit_title_, sizeof(edit_title_), "Key for %s", m->id);
            edit_buf_[0] = 0;
            edit_err_[0] = 0;
        } else if (ch == 'n') {
            edit_ = true;
            edit_kind_ = 2;
            snprintf(edit_title_, sizeof(edit_title_), "Name for %s", m->id);
            snprintf(edit_buf_, sizeof(edit_buf_), "%s", c ? c->name : "");
            edit_err_[0] = 0;
        } else if (ch == 'f' || in.ch == '*') {
            MeterConf mc;
            if (c) mc = *c;
            else {
                memset(&mc, 0, sizeof(mc));
                snprintf(mc.id, sizeof(mc.id), "%s", m->id);
            }
            mc.starred = !mc.starred;
            meterconf_put(mc);
            toast(mc.starred ? "Starred" : "Unstarred", T->warn, now);
        } else if (ch == 't') {
            track(m->id);
        } else if (ch == 'r') {
            screen_ = Screen::Raw;
        }
    }
}

void UiView::draw_raw(uint32_t now) {
    Meter* m = meters_find(detail_id_);
    if (!m) {
        screen_ = back_;
        return;
    }
    font(&fonts::DejaVu9);
    char head[48];
    snprintf(head, sizeof(head), "%s  %s  %u bytes", m->id, link_mode_name((LinkMode)m->frame_mode), m->frame_len);
    text(4, CY + 3, head, T->text);
    font(&fonts::Font0);
    static const char HX[] = "0123456789ABCDEF";
    int y = CY + 16, per = 13;  // 13 bytes per line (78 px of hex pairs + spacing)
    for (int i = 0; i < m->frame_len && y < H - HINT_H - 8; i += per) {
        char line[64];
        int o = 0;
        for (int k = 0; k < per && i + k < m->frame_len; ++k) {
            line[o++] = HX[m->frame[i + k] >> 4];
            line[o++] = HX[m->frame[i + k] & 15];
            if (k != per - 1) line[o++] = (k % 4 == 3) ? ' ' : 0x7F;
        }
        line[o] = 0;
        for (int k = 0; k < o; ++k) if (line[k] == 0x7F) line[k] = ' ';
        text(4, y, line, i == 0 ? T->accent : T->text);
        y += 9;
    }
    (void)now;
    draw_hints("DEL back");
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
enum SetupItem {
    S_BAND, S_WIFI, S_QR, S_SOUND, S_BEEP, S_BRIGHT, S_DIM, S_SERIAL, S_SD, S_THEME, S_RADIO, S_BW, S_CLEAR, S_ABOUT,
    S_COUNT
};

static const char* setup_label(int i) {
    static const char* const L[] = { "Band", "WiFi", "Connect phone (QR)", "Sounds", "Beep every telegram",
                                     "Brightness", "Dim display", "USB serial output", "SD card log", "Theme",
                                     "Radio hardware", "SX1262 bandwidth", "Clear meter list", "About" };
    return L[i];
}

static void setup_value(int i, char* out, size_t cap) {
    static const char* const BANDS[] = { "C1/T1", "S1", "Alternate" };
    static const char* const WIFI[] = { "Off", "Access point", "Join network", "AP + network" };
    static const char* const SER[] = { "Off", "Log", "JSON", "rtl_wmbus" };
    static const char* const HW[] = { "Auto", "SX1262", "CC1101" };
    switch (i) {
    case S_BAND: snprintf(out, cap, "%s", BANDS[g_cfg.band_mode % 3]); break;
    case S_WIFI: snprintf(out, cap, "%s", WIFI[g_cfg.wifi_mode % 4]); break;
    case S_QR: snprintf(out, cap, ">"); break;
    case S_SOUND: snprintf(out, cap, "%s", g_cfg.sound ? "On" : "Off"); break;
    case S_BEEP: snprintf(out, cap, "%s", g_cfg.beep_all ? "On" : "Off"); break;
    case S_BRIGHT: snprintf(out, cap, "%d%%", g_cfg.brightness * 100 / 255); break;
    case S_DIM:
        if (!g_cfg.dim_s) snprintf(out, cap, "Never");
        else if (g_cfg.dim_s < 60) snprintf(out, cap, "%us", g_cfg.dim_s);
        else snprintf(out, cap, "%um", g_cfg.dim_s / 60);
        break;
    case S_SERIAL: snprintf(out, cap, "%s", SER[g_cfg.serial_out % 4]); break;
    case S_SD: snprintf(out, cap, "%s", !sdlog_card_ok() ? "No card" : g_cfg.sd_log ? "On" : "Off"); break;
    case S_THEME: snprintf(out, cap, "%s", THEMES[g_cfg.theme % 4].name); break;
    case S_RADIO: snprintf(out, cap, "%s", HW[g_cfg.radio_hw % 3]); break;
    case S_BW: snprintf(out, cap, "%.0f kHz", (double)settings_sx_bw(g_cfg.sx_bw_idx)); break;
    case S_CLEAR: snprintf(out, cap, "%d", meters_count()); break;
    case S_ABOUT: snprintf(out, cap, "%s", WMB_VERSION); break;
    default: out[0] = 0;
    }
}

void UiView::draw_setup(uint32_t now) {
    const int rh = 15;
    int vis = CH / rh;
    if (setup_sel_ < setup_top_) setup_top_ = setup_sel_;
    if (setup_sel_ >= setup_top_ + vis) setup_top_ = setup_sel_ - vis + 1;
    for (int r = 0; r < vis && setup_top_ + r < S_COUNT; ++r) {
        int i = setup_top_ + r;
        int y = CY + 1 + r * rh;
        bool sel = i == setup_sel_;
        if (sel) g_->fillSmoothRoundRect(2, y, W - 8, rh - 1, 5, T->panel2);
        font(&fonts::DejaVu9);
        text(9, y + 3, setup_label(i), sel ? T->text : T->muted);
        char v[32];
        setup_value(i, v, sizeof(v));
        if (i == S_BRIGHT) {
            int bw = 50, bx = W - 14 - bw;
            g_->fillRoundRect(bx, y + 5, bw, 4, 2, T->line);
            g_->fillRoundRect(bx, y + 5, bw * g_cfg.brightness / 255, 4, 2, T->accent);
        } else {
            uint32_t vc = sel ? T->accent : T->text;
            if (i == S_CLEAR) vc = T->bad;
            text(W - 12, y + 3, v, vc, lgfx::top_right);
            if (sel && i != S_QR && i != S_CLEAR && i != S_ABOUT) {
                g_->fillTriangle(W - 9, y + 4, W - 9, y + 10, W - 6, y + 7, T->accent);
            }
        }
    }
    int n = S_COUNT;
    if (n > vis) {
        int th = CH * vis / n, ty = CY + (CH - th) * setup_top_ / (n - vis);
        g_->fillRoundRect(W - 3, ty, 2, th, 1, T->line);
    }
    (void)now;
    draw_hints(";. select  ,/ change");
}

void UiView::setup_change(int item, int dir, uint32_t now) {
    auto cyc = [dir](int v, int n) { return (v + dir + n) % n; };
    switch (item) {
    case S_BAND:
        g_cfg.band_mode = (uint8_t)cyc(g_cfg.band_mode, 3);
        app_apply_radio_band();
        break;
    case S_WIFI:
        g_cfg.wifi_mode = (uint8_t)cyc(g_cfg.wifi_mode, 4);
        net_apply_settings();
        break;
    case S_QR:
        back_ = Screen::Setup;
        screen_ = Screen::Qr;
        return;
    case S_SOUND: g_cfg.sound = !g_cfg.sound; break;
    case S_BEEP: g_cfg.beep_all = !g_cfg.beep_all; break;
    case S_BRIGHT: {
        int b = g_cfg.brightness + dir * 25;
        g_cfg.brightness = (uint8_t)(b < 10 ? 10 : b > 255 ? 255 : b);
        board_set_brightness(g_cfg.brightness);
        break;
    }
    case S_DIM: {
        static const uint16_t D[] = { 0, 30, 60, 120, 300, 900 };
        int k = 0;
        for (int i = 0; i < 6; ++i) if (D[i] == g_cfg.dim_s) k = i;
        g_cfg.dim_s = D[cyc(k, 6)];
        break;
    }
    case S_SERIAL: g_cfg.serial_out = (uint8_t)cyc(g_cfg.serial_out, 4); break;
    case S_SD: g_cfg.sd_log = !g_cfg.sd_log; break;
    case S_THEME:
        g_cfg.theme = (uint8_t)cyc(g_cfg.theme, 4);
        set_theme(g_cfg.theme);
        break;
    case S_RADIO:
        g_cfg.radio_hw = (uint8_t)cyc(g_cfg.radio_hw, 3);
        toast("Reboot to switch radio hardware", T->warn, now);
        break;
    case S_BW:
        g_cfg.sx_bw_idx = (uint8_t)cyc(g_cfg.sx_bw_idx, settings_sx_bw_count());
        toast("Reboot to apply the bandwidth", T->warn, now);
        break;
    case S_CLEAR:
        confirm_ = true;
        confirm_what_ = 1;
        return;
    default: return;
    }
    settings_save();
}

void UiView::input_setup(const UiInput& in, uint32_t now) {
    if (in.key == UiKey::Up && setup_sel_ > 0) setup_sel_--;
    else if (in.key == UiKey::Down && setup_sel_ < S_COUNT - 1) setup_sel_++;
    else if (in.key == UiKey::Right || in.key == UiKey::Enter) setup_change(setup_sel_, 1, now);
    else if (in.key == UiKey::Left) setup_change(setup_sel_, -1, now);
}

void UiView::draw_qr(uint32_t now) {
    NetStatus ns;
    net_status(&ns);
    char qr[160];
    const char* url_ip = ns.ap_on ? ns.ap_ip : ns.sta_ip;
    if (ns.ap_on) {
        const char* pass = strlen(g_cfg.ap_pass) >= 8 ? g_cfg.ap_pass : "";
        snprintf(qr, sizeof(qr), "WIFI:T:%s;S:%s;P:%s;;", pass[0] ? "WPA" : "nopass", ns.ap_ssid, pass);
    } else {
        snprintf(qr, sizeof(qr), "http://%s/", url_ip[0] ? url_ip : "wmbuster.local");
    }
    int qs = CH - 4;
    g_->fillRect(3, CY + 2, qs, qs, 0xFFFFFF);
    g_->qrcode(qr, 5, CY + 4, qs - 4, 4);
    int x = qs + 10;
    font(&fonts::DejaVu9);
    int y = CY + 4;
    if (ns.ap_on) {
        text(x, y, "1. Scan to join WiFi", T->muted);
        font(&fonts::DejaVu12);
        char s[40];
        snprintf(s, sizeof(s), "%s", ns.ap_ssid);
        fit(s, W - x - 3);
        text(x, y + 12, s, T->text);
        font(&fonts::DejaVu9);
        snprintf(s, sizeof(s), "pw: %s", strlen(g_cfg.ap_pass) >= 8 ? g_cfg.ap_pass : "(open)");
        fit(s, W - x - 3);
        text(x, y + 28, s, T->muted);
        y += 46;
    } else if (!ns.sta_connected) {
        text(x, y, "WiFi is off", T->warn);
        text(x, y + 12, "Enable it in Setup", T->muted);
        y += 30;
    }
    text(x, y, ns.ap_on ? "2. Then open" : "Open in a browser", T->muted);
    font(&fonts::DejaVu12);
    char url[40];
    snprintf(url, sizeof(url), "%s", url_ip[0] ? url_ip : "-");
    text(x, y + 12, url, T->accent);
    font(&fonts::DejaVu9);
    char host[40];
    snprintf(host, sizeof(host), "%s.local", g_cfg.hostname[0] ? g_cfg.hostname : "wmbuster");
    fit(host, W - x - 3);
    text(x, y + 28, host, T->muted);
    (void)now;
    draw_hints("DEL back");
}

// ---------------------------------------------------------------------------
// Text entry and confirmation
// ---------------------------------------------------------------------------
void UiView::draw_edit() {
    int x = 10, y = 22, w = W - 20, h = 88;
    g_->fillSmoothRoundRect(x + 2, y + 3, w, h, 8, 0x000000);
    g_->fillSmoothRoundRect(x, y, w, h, 8, T->panel);
    g_->drawRoundRect(x, y, w, h, 8, T->accent);
    font(&fonts::DejaVu12);
    text(x + 10, y + 7, edit_title_, T->text);
    g_->fillSmoothRoundRect(x + 8, y + 26, w - 16, 22, 5, T->bg);
    font(edit_kind_ == 1 ? (const lgfx::IFont*)&fonts::Font0 : (const lgfx::IFont*)&fonts::DejaVu12);
    char shown[48];
    snprintf(shown, sizeof(shown), "%s", edit_buf_);
    int maxw = w - 30;
    const char* p = shown;
    while (*p && tw(p) > maxw) p++;
    text(x + 13, y + (edit_kind_ == 1 ? 34 : 31), p, T->text);
    int cx = x + 13 + tw(p);
    if ((millis() / 450) & 1) g_->fillRect(cx + 1, y + 30, 2, 14, T->accent);
    font(&fonts::DejaVu9);
    if (edit_err_[0]) text(x + 10, y + 54, edit_err_, T->bad);
    else if (edit_kind_ == 1) {
        char cnt[40];
        snprintf(cnt, sizeof(cnt), "%u/32 hex digits (16 = DES)", (unsigned)strlen(edit_buf_));
        text(x + 10, y + 54, cnt, T->muted);
    } else {
        text(x + 10, y + 54, "Up to 23 characters", T->muted);
    }
    text(x + 10, y + 69, "ENTER save   DEL erase   ` cancel", T->muted);
}

void UiView::input_edit(const UiInput& in_raw, uint32_t now) {
    UiInput in = in_raw;
    // Arrow keys share their key with punctuation: type it in text fields.
    if ((in.key == UiKey::Up || in.key == UiKey::Down || in.key == UiKey::Left || in.key == UiKey::Right) && in.ch)
        in.key = UiKey::Char;
    size_t n = strlen(edit_buf_);
    if (in.key == UiKey::Esc) {
        edit_ = false;
        return;
    }
    if (in.key == UiKey::Back) {
        if (n) edit_buf_[n - 1] = 0;
        edit_err_[0] = 0;
        return;
    }
    if (in.key == UiKey::Char) {
        char c = in.ch;
        size_t cap = edit_kind_ == 1 ? 32 : 23;
        if (edit_kind_ == 1) {
            if (c >= 'a' && c <= 'f') c = (char)(c - 32);
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return;
        }
        if (n < cap && c >= 32 && c < 127) {
            edit_buf_[n] = c;
            edit_buf_[n + 1] = 0;
        }
        edit_err_[0] = 0;
        return;
    }
    if (in.key != UiKey::Enter) return;
    const MeterConf* old = meterconf_find(detail_id_);
    MeterConf mc;
    if (old) mc = *old;
    else {
        memset(&mc, 0, sizeof(mc));
        snprintf(mc.id, sizeof(mc.id), "%s", detail_id_);
    }
    if (edit_kind_ == 1) {
        if (n == 0) mc.key_len = 0;
        else if (!parse_key_hex(edit_buf_, mc.key, &mc.key_len)) {
            snprintf(edit_err_, sizeof(edit_err_), "Need 32 (or 16) hex digits");
            return;
        }
    } else {
        snprintf(mc.name, sizeof(mc.name), "%s", edit_buf_);
    }
    if (!meterconf_put(mc)) {
        snprintf(edit_err_, sizeof(edit_err_), "Meter table full");
        return;
    }
    app_meter_config_changed(mc.id);
    mqtt_rediscover(mc.id);
    edit_ = false;
    const Meter* m = meters_find(detail_id_);
    if (edit_kind_ == 1 && m) {
        bool ok = m->status == DecodeStatus::Ok;
        toast(ok ? "Key works, telegram decoded" : n ? "Saved - waiting for a telegram" : "Key removed",
              ok ? T->ok : T->warn, now);
    } else {
        toast("Name saved", T->ok, now);
    }
}

void UiView::draw_confirm() {
    int x = 26, y = 36, w = W - 52, h = 56;
    g_->fillSmoothRoundRect(x, y, w, h, 8, T->panel);
    g_->drawRoundRect(x, y, w, h, 8, T->bad);
    font(&fonts::DejaVu12);
    text(W / 2, y + 10, "Clear meter list?", T->text, lgfx::top_center);
    font(&fonts::DejaVu9);
    text(W / 2, y + 30, "Names and keys are kept.", T->muted, lgfx::top_center);
    text(W / 2, y + 42, "ENTER yes   DEL no", T->muted, lgfx::top_center);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void UiView::on_telegram(uint32_t now) {
    const AppEvent& e = app_last_event();
    rx_flash_ = now;
    if (track_id_[0] && !strcmp(e.id, track_id_)) {
        const Meter* m = meters_find(track_id_);
        if (m) {
            if (hist_n_ == 64) {
                memmove(hist_, hist_ + 1, sizeof(hist_[0]) * 63);
                hist_n_ = 63;
            }
            hist_[hist_n_++] = m->rssi;
            track_seen_ = now;
            if (screen_ == Screen::Hunt && hunt_beep_) {
                int hz = 300 + (m->rssi + 120) * 35;
                ui_hook_tone((uint16_t)(hz < 300 ? 300 : hz > 3500 ? 3500 : hz), 40);
            }
        }
    }
    if (e.is_new) {
        char msg[48];
        const Meter* m = meters_find(e.id);
        snprintf(msg, sizeof(msg), "New meter %s %s", e.id, m ? m->mfct : "");
        toast(msg, T->accent, now);
    } else if (e.alarm && e.starred) {
        const Meter* m = meters_find(e.id);
        char msg[48];
        snprintf(msg, sizeof(msg), "%s: %s", e.id, m ? m->status_txt : "alarm");
        toast(msg, T->bad, now);
    }
    dirty_ = true;
}

void UiView::input(const UiInput& in, uint32_t now) {
    dirty_ = true;
    if (edit_) {
        input_edit(in, now);
        return;
    }
    if (confirm_) {
        if (in.key == UiKey::Enter) {
            meters_clear();
            feed_clear();
            sel_ = top_ = live_top_ = 0;
            toast("Meter list cleared", T->ok, now);
        }
        confirm_ = false;
        return;
    }
    // Global keys.
    if (in.key == UiKey::Tab || (in.key == UiKey::Char && in.ch >= '1' && in.ch <= '5')) {
        int next = in.key == UiKey::Tab ? ((int)(screen_ < Screen::TabCount ? screen_ : back_) + 1) % (int)Screen::TabCount
                                         : in.ch - '1';
        screen_ = (Screen)next;
        return;
    }
    switch (screen_) {
    case Screen::Meters:
        if (in.key == UiKey::Left || in.key == UiKey::Right) {
            screen_ = in.key == UiKey::Right ? Screen::Live : Screen::Setup;
            return;
        }
        input_meters(in);
        break;
    case Screen::Live:
    case Screen::Hunt:
    case Screen::Stats:
        if (in.key == UiKey::Left || in.key == UiKey::Right) {
            int s = (int)screen_ + (in.key == UiKey::Right ? 1 : -1);
            screen_ = (Screen)((s + (int)Screen::TabCount) % (int)Screen::TabCount);
            return;
        }
        if (screen_ == Screen::Live) input_live(in);
        else if (screen_ == Screen::Hunt) {
            if (in.key == UiKey::Char && (in.ch | 0x20) == 'b') hunt_beep_ = !hunt_beep_;
            else if (in.key == UiKey::Enter && track_id_[0]) open_meter(track_id_);
        }
        break;
    case Screen::Setup:
        if (in.key == UiKey::Back || in.key == UiKey::Esc) {
            screen_ = Screen::Meters;
            return;
        }
        input_setup(in, now);
        break;
    case Screen::Detail:
        input_detail(in, now);
        break;
    case Screen::Raw:
    case Screen::Qr:
        if (in.key == UiKey::Back || in.key == UiKey::Esc || in.key == UiKey::Left || in.key == UiKey::Enter)
            screen_ = screen_ == Screen::Raw ? Screen::Detail : Screen::Setup;
        break;
    default: break;
    }
}

bool UiView::draw(uint32_t now) {
    G = g_;
    const AppEvent& e = app_last_event();
    if (e.seq != last_seq_) {
        last_seq_ = e.seq;
        on_telegram(now);
    }
    // Sample the telegram rate for the stats graph every 5 s.
    if (now - rate_ms_ >= 5000) {
        rate_ms_ = now;
        if (rate_n_ == 48) {
            memmove(rate_, rate_ + 1, sizeof(rate_[0]) * 47);
            rate_n_ = 47;
        }
        rate_[rate_n_++] = (uint16_t)g_app.per_min;
        if (screen_ == Screen::Stats) dirty_ = true;
    }
    bool animate = now - rx_flash_ < 300 || toast_[0] || edit_ ||
                   ((screen_ == Screen::Meters || screen_ == Screen::Live) && meters_count() == 0);
    if (!dirty_ && !animate && now - last_draw_ < 1000) return false;
    if (animate && !dirty_ && now - last_draw_ < 40) return false;
    dirty_ = false;
    last_draw_ = now;
    g_->fillScreen(T->bg);
    switch (screen_) {
    case Screen::Meters: draw_meters(now); break;
    case Screen::Live: draw_live(now); break;
    case Screen::Hunt: draw_hunt(now); break;
    case Screen::Stats: draw_stats(now); break;
    case Screen::Setup: draw_setup(now); break;
    case Screen::Detail: draw_detail(now); break;
    case Screen::Raw: draw_raw(now); break;
    case Screen::Qr: draw_qr(now); break;
    default: break;
    }
    draw_status_bar(now);
    draw_toast(now);
    if (edit_) draw_edit();
    if (confirm_) draw_confirm();
    return true;
}

} // namespace wmb
