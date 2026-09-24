// wM-Buster ADV — graphics backend selection for the UI. On the device the
// UI draws into an M5Canvas; the host preview (test/ui_preview) compiles the
// same code against LovyanGFX's sprite on Linux.
// GPL-3.0
#pragma once

#if defined(ARDUINO)
#include <M5GFX.h>
#else
#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include "lgfx/v1/platforms/device.hpp"
#include "lgfx/v1/platforms/common.hpp"
#include "lgfx/v1/LGFXBase.hpp"
#include "lgfx/v1/LGFX_Sprite.hpp"
#endif

namespace wmb {
using Gfx = lgfx::LGFX_Sprite;
}
