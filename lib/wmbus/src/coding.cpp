// wM-Buster ADV — wM-Bus line codings.
// GPL-3.0
#include "wmbus/coding.h"

namespace wmb {

// Nibble -> 6-chip code (EN 13757-4 table 10).
static const uint8_t ENC_3OF6[16] = {
    0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
    0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29
};

// 6-chip code -> nibble, 0xFF when invalid. Generated at compile time.
struct Dec3of6 {
    uint8_t t[64];
    constexpr Dec3of6() : t() {
        for (int i = 0; i < 64; ++i) t[i] = 0xFF;
        for (int i = 0; i < 16; ++i) t[ENC_3OF6[i]] = (uint8_t)i;
    }
};
static constexpr Dec3of6 DEC_3OF6{};

static inline uint8_t read_bits6(const uint8_t* enc, size_t bit) {
    size_t byte = bit >> 3;
    unsigned off = (unsigned)(bit & 7);
    uint16_t w = (uint16_t)((enc[byte] << 8) | (off > 2 ? enc[byte + 1] : 0));
    return (uint8_t)((w >> (10 - off)) & 0x3F);
}

bool coding_3of6_decode(const uint8_t* enc, size_t enc_len, uint8_t* out, size_t n) {
    if (coding_3of6_encoded_len(n) > enc_len) return false;
    size_t bit = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t hi = DEC_3OF6.t[read_bits6(enc, bit)];
        uint8_t lo = DEC_3OF6.t[read_bits6(enc, bit + 6)];
        if ((hi | lo) & 0xF0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        bit += 12;
    }
    return true;
}

size_t coding_3of6_encode(const uint8_t* in, size_t n, uint8_t* out, size_t out_max) {
    size_t len = coding_3of6_encoded_len(n);
    if (len > out_max) return 0;
    for (size_t i = 0; i < len; ++i) out[i] = 0;
    size_t bit = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t codes[2] = { ENC_3OF6[in[i] >> 4], ENC_3OF6[in[i] & 0x0F] };
        for (int c = 0; c < 2; ++c) {
            for (int b = 5; b >= 0; --b) {
                if (codes[c] & (1 << b)) out[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
                bit++;
            }
        }
    }
    return len;
}

bool coding_manchester_decode(const uint8_t* chips, size_t chips_len, uint8_t* out, size_t n, bool invert) {
    if (n * 2 > chips_len) return false;
    for (size_t i = 0; i < n; ++i) {
        uint16_t w = (uint16_t)((chips[2 * i] << 8) | chips[2 * i + 1]);
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) {
            uint8_t pair = (uint8_t)((w >> (14 - 2 * b)) & 0x3);
            if (pair == 0x2) v = (uint8_t)((v << 1) | (invert ? 0 : 1));
            else if (pair == 0x1) v = (uint8_t)((v << 1) | (invert ? 1 : 0));
            else return false;
        }
        out[i] = v;
    }
    return true;
}

} // namespace wmb
