// wM-Buster ADV — host unit tests for the PHY, crypto and formula layers.
// GPL-3.0
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wmbus/aes.h"
#include "wmbus/coding.h"
#include "wmbus/crc.h"
#include "wmbus/dv.h"
#include "wmbus/engine.h"
#include "wmbus/formula.h"
#include "wmbus/frame.h"
#include "wmbus/lookup.h"

using namespace wmb;

static int g_fail = 0, g_ok = 0;
#define CHECK(c) do { if (c) g_ok++; else { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool eq_hex(const uint8_t* b, size_t n, const char* hex) {
    uint8_t x[64];
    size_t m = hex_to_bytes(hex, x, sizeof(x));
    return m == n && memcmp(b, x, n) == 0;
}

// Build an on-air frame format A (with CRCs) from a CRC-less frame.
static size_t add_crcs_a(const uint8_t* in, size_t len, uint8_t* out) {
    size_t o = 0, i = 0;
    auto put_crc = [&](size_t from, size_t n) {
        uint16_t c = crc16_en13757(out + from, n);
        out[o++] = (uint8_t)(c >> 8);
        out[o++] = (uint8_t)c;
    };
    memcpy(out, in, 10);
    o = 10;
    i = 10;
    put_crc(0, 10);
    while (i < len) {
        size_t n = len - i < 16 ? len - i : 16;
        size_t from = o;
        memcpy(out + o, in + i, n);
        o += n;
        i += n;
        put_crc(from, n);
    }
    return o;
}

static size_t add_crcs_b(const uint8_t* in, size_t len, uint8_t* out) {
    // L counts the CRC bytes in format B (single block when <= 126 bytes).
    memcpy(out, in, len);
    out[0] = (uint8_t)(len + 2 - 1);
    uint16_t c = crc16_en13757(out, len);
    out[len] = (uint8_t)(c >> 8);
    out[len + 1] = (uint8_t)c;
    return len + 2;
}

static void test_crc_coding() {
    const uint8_t v[] = { 0x12, 0x34, 0x56 };
    CHECK(crc16_en13757(v, 0) == 0xFFFF);
    // 3 out of 6: round trip including odd lengths.
    for (size_t n = 1; n < 40; ++n) {
        uint8_t in[40], enc[80], dec[40];
        for (size_t i = 0; i < n; ++i) in[i] = (uint8_t)(i * 37 + 11);
        size_t el = coding_3of6_encode(in, n, enc, sizeof(enc));
        CHECK(el == (n * 3 + 1) / 2);
        CHECK(coding_3of6_decode(enc, el, dec, n));
        CHECK(memcmp(in, dec, n) == 0);
        CHECK(!coding_3of6_decode(enc, el - 1, dec, n));
    }
    uint8_t junk[3] = { 0xFF, 0xFF, 0xFF }, out[2];
    CHECK(!coding_3of6_decode(junk, 3, out, 2));
    // Manchester
    uint8_t chips[4] = { 0xA6, 0x5A, 0x55, 0xAA }; // 1011 0100 ... -> "10 10 01 10" etc.
    uint8_t mb[2];
    CHECK(coding_manchester_decode(chips, 4, mb, 2, false));
    CHECK(mb[0] == 0xD3 && mb[1] == 0x0F);
}

static void test_aes() {
    // FIPS-197 C.1
    uint8_t key[16], pt[16], ct[16], out[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", key, 16);
    hex_to_bytes("00112233445566778899aabbccddeeff", pt, 16);
    hex_to_bytes("69c4e0d86a7b0430d8cdb78070b4c55a", ct, 16);
    Aes128 a;
    aes128_init(&a, key);
    aes128_encrypt_block(&a, pt, out);
    CHECK(memcmp(out, ct, 16) == 0);
    aes128_decrypt_block(&a, ct, out);
    CHECK(memcmp(out, pt, 16) == 0);
    // RFC 4493 CMAC examples
    uint8_t k2[16], mac[16], msg[64];
    hex_to_bytes("2b7e151628aed2a6abf7158809cf4f3c", k2, 16);
    aes128_cmac(k2, msg, 0, mac);
    CHECK(eq_hex(mac, 16, "bb1d6929e95937287fa37d129b756746"));
    hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", msg, 16);
    aes128_cmac(k2, msg, 16, mac);
    CHECK(eq_hex(mac, 16, "070a16b46b4d4144f79bdd9dd04a287c"));
    hex_to_bytes("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411", msg, 40);
    aes128_cmac(k2, msg, 40, mac);
    CHECK(eq_hex(mac, 16, "dfa66747de9ae63030ca32611497c827"));
    // SP800-38A F.5.1 CTR
    uint8_t iv[16], ctr_in[16], ctr_out[16];
    hex_to_bytes("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, 16);
    hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", ctr_in, 16);
    aes128_ctr_crypt(k2, iv, ctr_in, ctr_out, 16);
    CHECK(eq_hex(ctr_out, 16, "874d6191b620e3261bef6864990db6ce"));
}

static void test_capture() {
    // A CRC-less telegram from the kamwater tests.
    const char* tg = "2A442D2C998734761B168D2091D37CAC21576C7802FF207100041308190000441308190000615B7F616713";
    uint8_t plain[80];
    size_t n = hex_to_bytes(tg, plain, sizeof(plain));
    CHECK(n > 11 && plain[0] + 1 == (int)n);

    // T1: 3-out-of-6 coded format A, captured with trailing noise.
    uint8_t onair[120], raw[255];
    size_t on = add_crcs_a(plain, n, onair);
    CHECK(on == frame_len_format_a(plain[0]));
    size_t el = coding_3of6_encode(onair, on, raw, sizeof(raw));
    for (size_t i = el; i < sizeof(raw); ++i) raw[i] = (uint8_t)rand();
    Frame f;
    size_t used = 0;
    CHECK(frame_from_capture(raw, sizeof(raw), RadioBand::CT, &f, &used) == CaptureStatus::Ok);
    CHECK(f.mode == LinkMode::T1 && f.len == n && memcmp(f.data, plain, n) == 0);
    CHECK(used == el);
    // The radio stops receiving after capture_expected_len() bytes.
    CHECK(capture_expected_len(raw, 2, RadioBand::CT) == 0);
    CHECK(capture_expected_len(raw, 3, RadioBand::CT) == (int)el);
    // Odd length frame (drop the last byte and fix L).
    uint8_t odd[80];
    memcpy(odd, plain, n - 1);
    odd[0] = (uint8_t)(n - 2);
    on = add_crcs_a(odd, n - 1, onair);
    el = coding_3of6_encode(onair, on, raw, sizeof(raw));
    for (size_t i = el; i < sizeof(raw); ++i) raw[i] = 0xFF;
    CHECK(frame_from_capture(raw, sizeof(raw), RadioBand::CT, &f, &used) == CaptureStatus::Ok);
    CHECK(f.len == n - 1 && memcmp(f.data + 1, odd + 1, n - 2) == 0);
    // Corrupt one byte -> CRC error.
    raw[20] ^= 0x2C;
    CaptureStatus st = frame_from_capture(raw, sizeof(raw), RadioBand::CT, &f, &used);
    CHECK(st == CaptureStatus::CrcError || st == CaptureStatus::BadCoding);

    // C1 frame format A: 54 CD + frame
    raw[0] = 0x54;
    raw[1] = 0xCD;
    on = add_crcs_a(plain, n, raw + 2);
    for (size_t i = on + 2; i < sizeof(raw); ++i) raw[i] = 0x00;
    CHECK(frame_from_capture(raw, sizeof(raw), RadioBand::CT, &f, &used) == CaptureStatus::Ok);
    CHECK(f.mode == LinkMode::C1 && f.format == FrameFormat::A && f.len == n);
    CHECK(capture_expected_len(raw, 3, RadioBand::CT) == (int)(2 + on) && used == 2 + on);
    // C1 frame format B: 54 3D + frame
    raw[1] = 0x3D;
    on = add_crcs_b(plain, n, raw + 2);
    CHECK(frame_from_capture(raw, sizeof(raw), RadioBand::CT, &f, &used) == CaptureStatus::Ok);
    CHECK(f.format == FrameFormat::B && f.len == n && memcmp(f.data + 1, plain + 1, n - 1) == 0);
    CHECK(capture_expected_len(raw, 3, RadioBand::CT) == (int)(2 + on));
    // Noise after a false sync: invalid 3of6 or a too small L-field.
    uint8_t noise[3] = { 0x00, 0x00, 0x00 };
    CHECK(capture_expected_len(noise, 3, RadioBand::CT) < 0);
    // Truncated capture
    CHECK(frame_from_capture(raw, 20, RadioBand::CT, &f, &used) == CaptureStatus::Truncated);

    // S1: Manchester coded format A (both chip polarities are accepted).
    on = add_crcs_a(plain, n, onair);
    for (int inv = 0; inv < 2; ++inv) {
        memset(raw, 0x55, sizeof(raw));
        for (size_t i = 0; i < on && 2 * i + 1 < sizeof(raw); ++i) {
            uint16_t chips = 0;
            for (int b = 7; b >= 0; --b) chips = (uint16_t)(chips << 2 | (((onair[i] >> b) & 1) ^ inv ? 2 : 1));
            raw[2 * i] = (uint8_t)(chips >> 8);
            raw[2 * i + 1] = (uint8_t)chips;
        }
        CHECK(capture_expected_len(raw, 23, RadioBand::S) == 0);
        CHECK(capture_expected_len(raw, 24, RadioBand::S) == (int)(2 * on));
        CHECK(frame_from_capture(raw, sizeof(raw), RadioBand::S, &f, &used) == CaptureStatus::Ok);
        CHECK(f.mode == LinkMode::S1 && f.len == n && memcmp(f.data + 1, plain + 1, n - 1) == 0);
    }

    // Analyzer input: with CRCs (A and B), without, wired.
    on = add_crcs_a(plain, n, onair);
    CHECK(frame_from_bytes(onair, on, &f) && f.len == n && f.format == FrameFormat::A);
    on = add_crcs_b(plain, n, onair);
    CHECK(frame_from_bytes(onair, on, &f) && f.len == n && f.format == FrameFormat::B);
    CHECK(frame_from_bytes(plain, n, &f) && f.len == n);
}

static bool fres(const char* id, FVal* out, void*) {
    if (!strcmp(id, "a_counter")) { *out = FVal{ 7, Unit::COUNTER, false, true }; return true; }
    if (!strcmp(id, "t_date")) { *out = FVal{ civil_to_unix(2020, 1, 31, 0, 0, 0), Unit::DateLT, false, true }; return true; }
    return false;
}

static void test_formula() {
    FVal v = formula_eval("(a_counter + 1counter) / 1 counter * 1 kwh", fres, nullptr);
    CHECK(v.ok && v.v == 8 && v.u == Unit::KWH);
    v = formula_eval("a_counter >> 1counter", fres, nullptr);
    CHECK(v.ok && v.v == 3);
    v = formula_eval("t_date - ((a_counter - 6counter) * 1 month)", fres, nullptr);
    int y, mo, d, h, mi, s;
    unix_to_civil(v.v, &y, &mo, &d, &h, &mi, &s);
    CHECK(v.ok && y == 2019 && mo == 12 && d == 31);
    v = formula_eval("t_date + 1 month", fres, nullptr);
    unix_to_civil(v.v, &y, &mo, &d, &h, &mi, &s);
    CHECK(v.ok && mo == 2 && d == 29); // last day maps to last day
    v = formula_eval("(1counter << (a_counter + 2counter)) * 1s", fres, nullptr);
    CHECK(v.ok && v.v == 512 && v.u == Unit::Second);
    CHECK(fabs(unit_convert(1, Unit::KWH, Unit::MJ) - 3.6) < 1e-9);
    CHECK(fabs(unit_convert(20, Unit::C, Unit::F) - 68) < 1e-9);
}

static void test_lookup() {
    static const LookupMap maps[] = { { 0x01, "DRY", TestBit::Set }, { 0x02, "REVERSE", TestBit::Set } };
    static const LookupRule rule = { "ERROR_FLAGS", MapType::BitToString, 0xFF, "OK", maps, 2 };
    char out[64];
    lookup_translate(&rule, 1, 0, out, sizeof(out));
    CHECK(!strcmp(out, "OK"));
    lookup_translate(&rule, 1, 0x13, out, sizeof(out));
    CHECK(!strcmp(out, "DRY ERROR_FLAGS_10 REVERSE"));
}

int main() {
    test_crc_coding();
    test_aes();
    test_capture();
    test_formula();
    test_lookup();
    printf("unit tests: %d passed, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
