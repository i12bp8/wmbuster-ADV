// wM-Buster ADV — the on-device user interface (240x135), independent of the
// input/output hardware so it can be rendered on a PC for previews.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ui_gfx.h"

namespace wmb {

enum class UiKey : uint8_t { None, Up, Down, Left, Right, Enter, Back, Tab, Esc, Char };

struct UiInput {
    UiKey key;
    char  ch;       // for UiKey::Char
};

enum class Screen : uint8_t { Meters = 0, Live, Hunt, Stats, Setup, TabCount, Detail, Raw, Qr };

// Hooks implemented by the platform glue (device: speaker, settings apply).
void ui_hook_tone(uint16_t hz, uint16_t ms);

class UiView {
public:
    void begin(Gfx* g);
    void set_theme(uint8_t idx);
    void input(const UiInput& in, uint32_t now);
    // Draws when something changed or an animation runs; true when the
    // canvas must be pushed to the display.
    bool draw(uint32_t now);
    void splash(const char* status, uint32_t now);
    void set_screen(Screen s);
    Screen screen() const { return screen_; }
    void open_meter(const char* id);   // detail view (preview / notifications)
    void track(const char* id);        // hunt a meter
    void force_redraw() { dirty_ = true; }

private:
    Gfx* g_ = nullptr;
    Screen screen_ = Screen::Meters;
    Screen back_ = Screen::Meters;
    bool dirty_ = true;
    uint32_t last_draw_ = 0;
    uint32_t last_seq_ = 0;
    uint32_t rx_flash_ = 0;

    // Meters list
    int sel_ = 0;
    int top_ = 0;
    float sel_y_ = 0;        // animated highlight position
    uint8_t sort_ = 0;
    // Live feed
    int live_top_ = 0;
    // Detail
    char detail_id_[9] = "";
    int detail_scroll_ = 0;
    // Hunt
    char track_id_[9] = "";
    int16_t hist_[64];
    uint8_t hist_n_ = 0;
    uint32_t track_seen_ = 0;
    bool hunt_beep_ = true;
    // Stats
    uint16_t rate_[48];
    uint8_t rate_n_ = 0;
    uint32_t rate_ms_ = 0;
    uint32_t rate_prev_ = 0;
    // Setup
    int setup_sel_ = 0;
    int setup_top_ = 0;
    // Text entry modal
    bool edit_ = false;
    char edit_title_[32];
    char edit_buf_[40];
    uint8_t edit_kind_ = 0;   // 1 key, 2 name
    char edit_err_[32];
    // Toast
    char toast_[48] = "";
    uint32_t toast_until_ = 0;
    uint32_t toast_color_ = 0;
    // Confirm
    bool confirm_ = false;
    uint8_t confirm_what_ = 0;

    void draw_status_bar(uint32_t now);
    void draw_hints(const char* text);
    void draw_meters(uint32_t now);
    void draw_live(uint32_t now);
    void draw_hunt(uint32_t now);
    void draw_stats(uint32_t now);
    void draw_setup(uint32_t now);
    void draw_detail(uint32_t now);
    void draw_raw(uint32_t now);
    void draw_qr(uint32_t now);
    void draw_edit();
    void draw_toast(uint32_t now);
    void draw_confirm();
    void draw_empty_radar(uint32_t now, int y0, int h);

    void input_meters(const UiInput& in);
    void input_live(const UiInput& in);
    void input_detail(const UiInput& in, uint32_t now);
    void input_setup(const UiInput& in, uint32_t now);
    void input_edit(const UiInput& in, uint32_t now);
    void setup_change(int item, int dir, uint32_t now);
    void on_telegram(uint32_t now);
    void toast(const char* msg, uint32_t color, uint32_t now);
};

} // namespace wmb
