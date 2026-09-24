// wM-Buster ADV — UI entry points for the firmware (keyboard + display glue).
// GPL-3.0
#pragma once

#include <stdint.h>

namespace wmb {

void ui_begin();
void ui_splash(const char* status);
void ui_loop();
void ui_apply_theme();

} // namespace wmb
