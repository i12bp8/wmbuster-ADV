// wM-Buster ADV — Cardputer-ADV board helpers: RF switch, alerts (speaker,
// RGB LED), battery and display brightness.
// GPL-3.0
#pragma once

#include <stdint.h>

namespace wmb {

struct AppEvent;

void board_begin();
void board_loop();

// Cap LoRa-1262: drive PI4IOE5V6408 P0 high to enable the antenna path.
bool board_rf_switch();

void board_telegram_alert(const AppEvent& e);
void board_click();                 // key feedback
void board_set_brightness(uint8_t b);
int  board_battery_pct();           // -1 when unknown
float board_battery_v();
bool board_charging();

// Display dimming: call on every user interaction.
void board_user_activity();
bool board_display_dimmed();

} // namespace wmb
