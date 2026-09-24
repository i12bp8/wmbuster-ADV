// wM-Buster ADV — UI glue: Cardputer keyboard -> UiView, canvas -> display.
// GPL-3.0
#include "ui.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <M5Unified.h>

#include "ui_view.h"
#include "../app/settings.h"
#include "../hw/board.h"

namespace wmb {

static M5Canvas* s_canvas = nullptr;
static UiView s_view;

void ui_hook_tone(uint16_t hz, uint16_t ms) {
    if (g_cfg.sound) M5.Speaker.tone(hz, ms);
}

void ui_begin() {
    M5.Display.setRotation(1);
    s_canvas = new M5Canvas(&M5.Display);
    s_canvas->setColorDepth(16);
    if (!s_canvas->createSprite(M5.Display.width(), M5.Display.height())) {
        Serial.println("[UI] canvas allocation failed");
    }
    s_canvas->setTextWrap(false);
    s_view.begin(s_canvas);
}

void ui_splash(const char* status) {
    if (!s_canvas) return;
    s_view.splash(status, millis());
    s_canvas->pushSprite(0, 0);
}

void ui_apply_theme() { s_view.set_theme(g_cfg.theme); }

static UiInput s_held{ UiKey::None, 0 };
static uint32_t s_held_since = 0, s_last_repeat = 0;

// Cardputer keyboard: ; . , / are the arrow keys, Del is back, ` is escape.
// The keyboard only reports changes, so held arrow keys repeat here.
static void poll_keys() {
    uint32_t now = millis();
    if (!M5Cardputer.Keyboard.isChange()) {
        bool nav = s_held.key == UiKey::Up || s_held.key == UiKey::Down || s_held.key == UiKey::Back;
        if (nav && M5Cardputer.Keyboard.isPressed() && now - s_held_since > 420 && now - s_last_repeat > 90) {
            s_last_repeat = now;
            board_user_activity();
            s_view.input(s_held, now);
        }
        return;
    }
    s_held.key = UiKey::None;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
    bool was_dimmed = board_display_dimmed();
    board_user_activity();
    if (was_dimmed) return;  // the first key only wakes the display
    board_click();
    UiInput in{ UiKey::None, 0 };
    if (ks.enter) in.key = UiKey::Enter;
    else if (ks.del) in.key = UiKey::Back;
    else if (ks.tab) in.key = UiKey::Tab;
    else if (!ks.word.empty()) {
        char c = ks.word[0];
        in.ch = c;  // text fields still get the punctuation
        if (c == ';') in.key = UiKey::Up;
        else if (c == '.') in.key = UiKey::Down;
        else if (c == ',') in.key = UiKey::Left;
        else if (c == '/') in.key = UiKey::Right;
        else if (c == '`' || c == '~') in.key = UiKey::Esc;
        else in.key = UiKey::Char;
    }
    if (in.key != UiKey::None) {
        s_view.input(in, now);
        s_held = in;
        s_held_since = now;
    }
}

void ui_loop() {
    if (!s_canvas) return;
    poll_keys();
    if (M5.BtnA.wasPressed()) {
        board_user_activity();
        s_view.input(UiInput{ UiKey::Enter, 0 }, millis());
    }
    if (s_view.draw(millis())) s_canvas->pushSprite(0, 0);
}

} // namespace wmb
