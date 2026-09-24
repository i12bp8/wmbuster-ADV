// wM-Buster ADV — Cardputer-ADV board helpers.
// GPL-3.0
#include "board.h"

#include <Arduino.h>
#include <M5Unified.h>

#include "config.h"
#include "../app/app.h"
#include "../app/settings.h"

namespace wmb {

static uint32_t s_led_off_ms = 0;
static uint32_t s_last_activity = 0;
static bool s_dimmed = false;
static int s_batt_pct = -1;
static float s_batt_v = 0;
static uint32_t s_batt_ms = 0;

void board_begin() {
    M5.Speaker.setVolume(90);
    M5.Led.setBrightness(40);
    M5.Led.setAllColor(0, 0, 0);
    M5.Led.display();
    board_set_brightness(g_cfg.brightness);
    s_last_activity = millis();
}

bool board_rf_switch() {
    // PI4IOE5V6408 on the internal I2C bus (the keyboard's bus, G8/G9).
    const uint32_t hz = 400000;
    const uint8_t addr = PI4IOE_I2C_ADDR;
    if (!M5.In_I2C.scanID(addr, hz)) return false;
    uint8_t hiz = M5.In_I2C.readRegister8(addr, 0x07, hz);        // output high impedance
    M5.In_I2C.writeRegister8(addr, 0x07, hiz & ~(1 << PI4IOE_PIN_RF_SW), hz);
    uint8_t dir = M5.In_I2C.readRegister8(addr, PI4IOE_REG_IO_DIR, hz);  // 1 = output
    M5.In_I2C.writeRegister8(addr, PI4IOE_REG_IO_DIR, dir | (1 << PI4IOE_PIN_RF_SW), hz);
    uint8_t out = M5.In_I2C.readRegister8(addr, PI4IOE_REG_OUTPUT, hz);
    bool ok = M5.In_I2C.writeRegister8(addr, PI4IOE_REG_OUTPUT, out | (1 << PI4IOE_PIN_RF_SW), hz);
    Serial.printf("[RF] antenna switch %s (PI4IOE @0x%02X)\n", ok ? "enabled" : "FAILED", addr);
    return ok;
}

static void led(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    M5.Led.setAllColor(r, g, b);
    M5.Led.display();
    s_led_off_ms = millis() + ms;
}

void board_telegram_alert(const AppEvent& e) {
    if (e.alarm) led(255, 20, 0, 180);
    else if (e.is_new) led(0, 90, 255, 120);
    else if (e.decoded) led(0, 255, 60, 60);
    else led(255, 150, 0, 60);
    if (!g_cfg.sound) return;
    if (e.starred && e.alarm) M5.Speaker.tone(3200, 150);
    else if (e.starred) M5.Speaker.tone(2600, 60);
    else if (e.is_new) M5.Speaker.tone(1900, 30);
    else if (g_cfg.beep_all) M5.Speaker.tone(1400, 12);
}

void board_click() {
    if (g_cfg.sound) M5.Speaker.tone(4000, 4);
}

void board_set_brightness(uint8_t b) { M5.Display.setBrightness(b); }

void board_user_activity() {
    s_last_activity = millis();
    if (s_dimmed) {
        s_dimmed = false;
        board_set_brightness(g_cfg.brightness);
    }
}

bool board_display_dimmed() { return s_dimmed; }

int board_battery_pct() { return s_batt_pct; }
float board_battery_v() { return s_batt_v; }
bool board_charging() { return M5.Power.isCharging() == m5::Power_Class::is_charging; }

void board_loop() {
    uint32_t now = millis();
    if (s_led_off_ms && (int32_t)(now - s_led_off_ms) >= 0) {
        s_led_off_ms = 0;
        M5.Led.setAllColor(0, 0, 0);
        M5.Led.display();
    }
    if (!s_dimmed && g_cfg.dim_s && now - s_last_activity > (uint32_t)g_cfg.dim_s * 1000u) {
        s_dimmed = true;
        board_set_brightness(g_cfg.brightness / 8 > 4 ? g_cfg.brightness / 8 : 4);
    }
    if (now - s_batt_ms > 5000 || !s_batt_ms) {
        s_batt_ms = now;
        int lvl = M5.Power.getBatteryLevel();
        s_batt_pct = (lvl >= 0 && lvl <= 100) ? lvl : -1;
        s_batt_v = M5.Power.getBatteryVoltage() / 1000.0f;
    }
}

} // namespace wmb
