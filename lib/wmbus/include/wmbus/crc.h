// CRC-16 per EN 13757 (poly 0x3D65, init 0x0000, MSB-first, final inversion).
// Independent implementation; behavior verified against wmbusmeters test data.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

uint16_t crc16_en13757(const uint8_t* data, size_t len);

} // namespace wmb
