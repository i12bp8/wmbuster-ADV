// wM-Buster ADV — wM-Bus line codings (EN 13757-4).
//  * T-mode: 3-out-of-6 — every nibble is sent as a 6-chip code with exactly
//    three ones, so one data byte occupies 12 chips (1.5 bytes on air).
//  * S-mode: Manchester — every bit is sent as two chips ("10" = 1, "01" = 0).
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

// Number of on-air bytes needed to carry n plain bytes in 3-out-of-6 coding.
inline size_t coding_3of6_encoded_len(size_t n) { return (n * 12 + 7) / 8; }

// Decode exactly n plain bytes from a 3-out-of-6 chip stream (codes may cross
// byte boundaries). Returns false if the input is too short or a chip group
// is not a valid code.
bool coding_3of6_decode(const uint8_t* enc, size_t enc_len, uint8_t* out, size_t n);

// Encode n plain bytes (used by tests / simulation). Returns encoded length.
size_t coding_3of6_encode(const uint8_t* in, size_t n, uint8_t* out, size_t out_max);

// Decode n plain bytes from a Manchester chip stream (2 chips per bit).
// invert=false: "10"→1, "01"→0. Returns false on an invalid chip pair.
bool coding_manchester_decode(const uint8_t* chips, size_t chips_len, uint8_t* out, size_t n, bool invert);

} // namespace wmb
