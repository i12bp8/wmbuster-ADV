// wM-Buster ADV — renders the device UI on a PC into PNG screenshots.
//   make -C test/ui_preview && ls test/ui_preview/out
// Real upstream test telegrams are decoded and fed to the real meter table.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ui/ui_view.h"
#include "wmbus/engine.h"
#include "app/meters.h"
#include "app/settings.h"
#include "app/app.h"
#include "../vectors/driver_tests.h"

extern uint32_t g_now_ms;
namespace wmb {
void preview_event(const char* id, bool is_new, bool alarm);
}
using namespace wmb;

static Decoder dec;

static const DriverTestVector* find_vec(const char* driver, const char* name) {
    for (unsigned i = 0; i < DRIVER_TEST_VECTORS_LEN; ++i) {
        const DriverTestVector& v = DRIVER_TEST_VECTORS[i];
        if (!strcmp(v.driver, driver) && (!name || !strcmp(v.name, name)) && !strchr(v.telegram, ',')) return &v;
    }
    return nullptr;
}

static Meter* add(const char* driver, const char* name, int rssi, uint32_t age_ms, const char* conf_name, bool star,
                  bool with_key, int repeats = 1) {
    const DriverTestVector* v = find_vec(driver, name);
    if (!v) {
        printf("no vector %s/%s\n", driver, name ? name : "*");
        return nullptr;
    }
    uint8_t bytes[400];
    size_t n = hex_to_bytes(v->telegram, bytes, sizeof(bytes));
    Frame f;
    if (!frame_from_bytes(bytes, n, &f)) return nullptr;
    f.mode = (rssi & 1) ? LinkMode::C1 : LinkMode::T1;
    DecodeOptions o;
    memset(&o, 0, sizeof(o));
    o.generic_fallback = true;
    o.simulated = true;
    uint8_t key[16];
    MeterConf c;
    memset(&c, 0, sizeof(c));
    bool has_key = with_key && strcmp(v->key, "NOKEY") != 0 && hex_to_bytes(v->key, key, 16) == 16;
    if (has_key) {
        o.key = key;
        o.key_len = 16;
    }
    wmbus_decode(&dec, f, o);
    if (has_key || conf_name || star) {
        snprintf(c.id, sizeof(c.id), "%s", dec.res.id);
        if (conf_name) snprintf(c.name, sizeof(c.name), "%s", conf_name);
        if (has_key) {
            memcpy(c.key, key, 16);
            c.key_len = 16;
        }
        c.starred = star;
        meterconf_put(c);
    }
    uint32_t base = g_now_ms;
    Meter* m = nullptr;
    for (int k = 0; k < repeats; ++k) {
        g_now_ms = base - age_ms - (uint32_t)(repeats - 1 - k) * 16000;
        bool is_new;
        double pos[2] = { 52.37, 4.89 };
        m = meters_update(f, dec.res, (int16_t)(rssi - (k % 3) * 2), pos, &is_new);
        FeedEntry e;
        memset(&e, 0, sizeof(e));
        e.ms = g_now_ms;
        snprintf(e.id, sizeof(e.id), "%s", dec.res.id);
        memcpy(e.mfct, dec.res.mfct, 4);
        e.rssi = (int16_t)rssi;
        e.mode = f.mode;
        e.status = dec.res.status;
        e.driver = dec.res.driver ? dec.res.driver->name : nullptr;
        if (m) memcpy(e.summary, m->summary, sizeof(e.summary));
        feed_add(e);
    }
    g_now_ms = base;
    return m;
}

static void save(Gfx& spr, const char* name) {
    static Gfx big;
    if (!big.getBuffer()) {
        big.setColorDepth(16);
        big.createSprite(240 * 3, 135 * 3);
    }
    spr.pushRotateZoom(&big, 360, 202, 0, 3, 3);
    size_t len = 0;
    void* png = big.createPng(&len, 0, 0, 720, 405);
    char path[128];
    snprintf(path, sizeof(path), "out/%s.png", name);
    FILE* f = fopen(path, "wb");
    if (f && png) fwrite(png, 1, len, f);
    if (f) fclose(f);
    free(png);
    printf("wrote %s\n", path);
}

static void shot(UiView& v, Gfx& spr, const char* name) {
    v.force_redraw();
    v.draw(g_now_ms);
    save(spr, name);
}

static void key(UiView& v, UiKey k, char ch = 0) { v.input(UiInput{ k, ch }, g_now_ms); }

int main() {
    mkdir("out", 0755);
    settings_defaults(&g_cfg);
    // A neighbourhood: plain and encrypted meters of different media.
    add("iperl", "MoreWater", -63, 4000, "Kitchen cold", true, true, 6);
    add("kamheat", "Kamstrup_303", -71, 12000, "Radiator loop", false, true, 4);
    add("amiplus", "MyElectricity1", -86, 30000, nullptr, false, true, 2);
    add("ei6500", "Smokey4", -79, 50000, "Hall smoke", false, true, 2);
    add("fhkvdataiv", nullptr, -95, 90000, nullptr, false, true, 1);
    add("multical21", nullptr, -101, 140000, nullptr, false, false, 3);
    add("apator162", "Wasser", -88, 200000, nullptr, false, true, 1);
    add("lansenth", nullptr, -69, 260000, "Bedroom", false, true, 1);
    add("hydrus", nullptr, -91, 400000, nullptr, false, false, 1);
    add("amiplus", "MyElectricity8", -97, 600000, nullptr, false, false, 1);  // encrypted, no key
    add("gwfwater", "Watererr", -83, 20000, "Garden tap", true, true, 2);
    g_app.frames = 1432;
    g_app.decoded = 1011;
    g_app.encrypted = 377;
    g_app.undecoded = 44;
    g_app.per_min = 23;
    g_app.crc_errors = 61;

    Gfx spr;
    spr.setColorDepth(16);
    spr.createSprite(240, 135);
    spr.setTextWrap(false);
    UiView v;
    v.begin(&spr);

    v.splash("SX1262 on Cap LoRa-1262 - listening", g_now_ms);
    save(spr, "00_splash");
    // Warm up the stats history.
    for (int i = 0; i < 40; ++i) {
        g_app.per_min = 12 + (i * 7) % 19;
        g_now_ms += 5000;
        v.draw(g_now_ms);
    }
    g_app.per_min = 23;
    v.set_screen(Screen::Meters);
    shot(v, spr, "01_meters");
    key(v, UiKey::Down);
    key(v, UiKey::Down);
    for (int i = 0; i < 8; ++i) { g_now_ms += 40; v.draw(g_now_ms); }
    shot(v, spr, "02_meters_sel");
    v.set_screen(Screen::Live);
    g_now_ms += 200;
    shot(v, spr, "03_live");
    // Hunt with some history.
    const Meter* m0 = meters_at(0);
    v.track(m0->id);
    for (int i = 0; i < 30; ++i) {
        Meter* m = meters_find(m0->id);
        m->rssi = (int16_t)(-95 + i + (i % 4) * 2);
        if (m->rssi > m->rssi_best) m->rssi_best = m->rssi;
        preview_event(m0->id, false, false);
        g_now_ms += 700;
        v.draw(g_now_ms);
    }
    g_now_ms += 400;
    shot(v, spr, "04_hunt");
    v.set_screen(Screen::Stats);
    shot(v, spr, "05_stats");
    v.set_screen(Screen::Setup);
    shot(v, spr, "06_setup");
    key(v, UiKey::Down);
    key(v, UiKey::Down);
    key(v, UiKey::Enter);
    shot(v, spr, "07_qr");
    key(v, UiKey::Back);
    // Detail of a decoded meter, an encrypted one, key entry, raw.
    v.open_meter(meters_at(1)->id);
    shot(v, spr, "08_detail");
    key(v, UiKey::Down);
    key(v, UiKey::Down);
    key(v, UiKey::Down);
    shot(v, spr, "09_detail_scroll");
    for (int i = 0; i < meters_count(); ++i) {
        if (meters_at(i)->status == DecodeStatus::Encrypted) {
            v.open_meter(meters_at(i)->id);
            break;
        }
    }
    shot(v, spr, "10_detail_encrypted");
    key(v, UiKey::Char, 'k');
    const char* partial = "B9E2C7D1F4";
    for (const char* p = partial; *p; ++p) key(v, UiKey::Char, *p);
    shot(v, spr, "11_key_entry");
    key(v, UiKey::Esc);
    key(v, UiKey::Char, 'r');
    shot(v, spr, "12_raw");
    // New meter toast on the meters list.
    v.set_screen(Screen::Meters);
    preview_event(meters_at(2)->id, true, false);
    g_now_ms += 100;
    shot(v, spr, "13_toast");
    // Themes.
    for (int t = 1; t < 4; ++t) {
        g_cfg.theme = (uint8_t)t;
        v.set_theme((uint8_t)t);
        g_now_ms += 5000;
        char nm[32];
        snprintf(nm, sizeof(nm), "14_theme_%d", t);
        shot(v, spr, nm);
    }
    g_cfg.theme = 0;
    v.set_theme(0);
    // Empty state.
    meters_clear();
    feed_clear();
    v.set_screen(Screen::Meters);
    g_now_ms += 700;
    shot(v, spr, "15_empty");
    return 0;
}
