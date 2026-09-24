#include "wmbus/crc.h"

namespace wmb {

static const uint16_t POLY = 0x3D65;

uint16_t crc16_en13757(const uint8_t* data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        for (int bit = 0; bit < 8; ++bit) {
            bool mix = ((crc & 0x8000) >> 8) ^ (b & 0x80);
            crc <<= 1;
            if (mix) crc ^= POLY;
            b <<= 1;
        }
    }
    return (uint16_t)~crc;
}

} // namespace wmb
