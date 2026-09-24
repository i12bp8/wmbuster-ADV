// wM-Buster ADV — wM-Bus data link frames.
// GPL-3.0
#include "wmbus/frame.h"
#include "wmbus/coding.h"
#include "wmbus/crc.h"

#include <string.h>

namespace wmb {

const char* link_mode_name(LinkMode m) {
    switch (m) {
    case LinkMode::C1: return "C1";
    case LinkMode::T1: return "T1";
    case LinkMode::S1: return "S1";
    case LinkMode::MBus: return "MBUS";
    default: return "?";
    }
}

const char* capture_status_name(CaptureStatus s) {
    switch (s) {
    case CaptureStatus::Ok: return "ok";
    case CaptureStatus::TooShort: return "too short";
    case CaptureStatus::BadCoding: return "bad coding";
    case CaptureStatus::Truncated: return "truncated";
    case CaptureStatus::CrcError: return "crc error";
    case CaptureStatus::BadLength: return "bad length";
    }
    return "?";
}

size_t frame_len_format_a(uint8_t l) {
    // First block: L C M A (10 bytes) + CRC, then 16 byte blocks + CRC each.
    if (l < 9) return (size_t)l + 1 + 2;
    size_t rest = (size_t)l - 9;
    size_t blocks = 1 + (rest + 15) / 16;
    return (size_t)l + 1 + 2 * blocks;
}

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

bool frame_trim_format_a(const uint8_t* in, size_t len, Frame* out) {
    if (len < 12) return false;
    if (crc16_en13757(in, 10) != be16(in + 10)) return false;
    size_t o = 0;
    memcpy(out->data, in, 10);
    o = 10;
    size_t pos = 12;
    for (; pos + 18 <= len; pos += 18) {
        if (crc16_en13757(in + pos, 16) != be16(in + pos + 16)) return false;
        if (o + 16 > sizeof(out->data)) return false;
        memcpy(out->data + o, in + pos, 16);
        o += 16;
    }
    if (pos + 2 < len) {
        size_t blen = len - 2 - pos;
        if (crc16_en13757(in + pos, blen) != be16(in + len - 2)) return false;
        if (o + blen > sizeof(out->data)) return false;
        memcpy(out->data + o, in + pos, blen);
        o += blen;
    }
    out->data[0] = (uint8_t)(o - 1);
    out->len = (uint16_t)o;
    out->format = FrameFormat::A;
    out->crc_checked = true;
    return true;
}

bool frame_trim_format_b(const uint8_t* in, size_t len, Frame* out) {
    if (len < 12) return false;
    size_t crc1, crc2;
    if (len <= 128) { crc1 = len - 2; crc2 = 0; }
    else { crc1 = 126; crc2 = len - 2; }
    if (crc16_en13757(in, crc1) != be16(in + crc1)) return false;
    if (crc1 > sizeof(out->data)) return false;
    memcpy(out->data, in, crc1);
    size_t o = crc1;
    if (crc2 > 0) {
        size_t from = crc1 + 2;
        if (crc2 < from) return false;
        size_t blen = crc2 - from;
        if (crc16_en13757(in + from, blen) != be16(in + crc2)) return false;
        if (o + blen > sizeof(out->data)) return false;
        memcpy(out->data + o, in + from, blen);
        o += blen;
    }
    out->data[0] = (uint8_t)(o - 1);
    out->len = (uint16_t)o;
    out->format = FrameFormat::B;
    out->crc_checked = true;
    return true;
}

static void frame_reset(Frame* f, LinkMode m) {
    f->len = 0;
    f->mode = m;
    f->format = FrameFormat::None;
    f->crc_checked = false;
    f->rssi = 0;
}

// C-mode: 54 CD (format A) or 54 3D (format B) followed by the frame.
static CaptureStatus decode_c(const uint8_t* raw, size_t raw_len, Frame* out, size_t* consumed) {
    frame_reset(out, LinkMode::C1);
    if (raw_len < 3 + 11) return CaptureStatus::TooShort;
    uint8_t l = raw[2];
    if (l < 10) return CaptureStatus::BadLength;
    bool fmt_a = raw[1] == 0xCD;
    size_t need = fmt_a ? frame_len_format_a(l) : frame_len_format_b(l);
    if (consumed) *consumed = 2 + need;
    if (2 + need > raw_len) return CaptureStatus::Truncated;
    bool ok = fmt_a ? frame_trim_format_a(raw + 2, need, out) : frame_trim_format_b(raw + 2, need, out);
    out->mode = LinkMode::C1;
    return ok ? CaptureStatus::Ok : CaptureStatus::CrcError;
}

// T-mode: 3-out-of-6 coded, frame format A (format B tried as a fallback).
static CaptureStatus decode_t(const uint8_t* raw, size_t raw_len, Frame* out, size_t* consumed) {
    frame_reset(out, LinkMode::T1);
    uint8_t head[2];
    if (raw_len < 3) return CaptureStatus::TooShort;
    if (!coding_3of6_decode(raw, raw_len, head, 2)) return CaptureStatus::BadCoding;
    uint8_t l = head[0];
    if (l < 10) return CaptureStatus::BadLength;
    uint8_t plain[WMB_FRAME_MAX];
    size_t need_a = frame_len_format_a(l);
    size_t enc_a = coding_3of6_encoded_len(need_a);
    if (consumed) *consumed = enc_a;
    if (enc_a <= raw_len && need_a <= sizeof(plain)) {
        if (!coding_3of6_decode(raw, raw_len, plain, need_a)) return CaptureStatus::BadCoding;
        if (frame_trim_format_a(plain, need_a, out)) {
            out->mode = LinkMode::T1;
            return CaptureStatus::Ok;
        }
    }
    size_t need_b = frame_len_format_b(l);
    size_t enc_b = coding_3of6_encoded_len(need_b);
    if (enc_b <= raw_len && coding_3of6_decode(raw, raw_len, plain, need_b) &&
        frame_trim_format_b(plain, need_b, out)) {
        if (consumed) *consumed = enc_b;
        out->mode = LinkMode::T1;
        return CaptureStatus::Ok;
    }
    return enc_a > raw_len ? CaptureStatus::Truncated : CaptureStatus::CrcError;
}

// S-mode: Manchester coded frame format A.
static CaptureStatus decode_s(const uint8_t* raw, size_t raw_len, Frame* out, size_t* consumed) {
    frame_reset(out, LinkMode::S1);
    uint8_t plain[WMB_FRAME_MAX];
    CaptureStatus best = CaptureStatus::BadCoding;
    for (int inv = 0; inv < 2; ++inv) {
        uint8_t head[1];
        if (!coding_manchester_decode(raw, raw_len, head, 1, inv != 0)) continue;
        uint8_t l = head[0];
        if (l < 10) { best = CaptureStatus::BadLength; continue; }
        size_t need = frame_len_format_a(l);
        if (consumed) *consumed = need * 2;
        if (need * 2 > raw_len || need > sizeof(plain)) { best = CaptureStatus::Truncated; continue; }
        if (!coding_manchester_decode(raw, raw_len, plain, need, inv != 0)) continue;
        if (frame_trim_format_a(plain, need, out)) {
            out->mode = LinkMode::S1;
            return CaptureStatus::Ok;
        }
        best = CaptureStatus::CrcError;
    }
    return best;
}

CaptureStatus frame_from_capture(const uint8_t* raw, size_t raw_len, RadioBand band,
                                 Frame* out, size_t* consumed) {
    if (consumed) *consumed = 0;
    if (band == RadioBand::S) return decode_s(raw, raw_len, out, consumed);
    if (raw_len >= 2 && raw[0] == 0x54 && (raw[1] == 0xCD || raw[1] == 0x3D)) {
        CaptureStatus st = decode_c(raw, raw_len, out, consumed);
        if (st == CaptureStatus::Ok) return st;
        // 0x54 is not a valid 3of6 code group start, so there is nothing else to try.
        return st;
    }
    return decode_t(raw, raw_len, out, consumed);
}

// Wired M-Bus long frame: 68 L L 68 C A CI ... CS 16
static bool parse_wired(const uint8_t* in, size_t len, Frame* out) {
    if (len < 9) return false;
    if (in[0] != 0x68 || in[3] != 0x68 || in[1] != in[2]) return false;
    size_t l = in[1];
    if (l + 6 > len) return false;
    uint8_t cs = 0;
    for (size_t i = 4; i < 4 + l; ++i) cs = (uint8_t)(cs + in[i]);
    if (cs != in[4 + l] || in[5 + l] != 0x16) return false;
    if (l > sizeof(out->data)) return false;
    frame_reset(out, LinkMode::MBus);
    memcpy(out->data, in + 4, l);
    out->len = (uint16_t)l;
    out->format = FrameFormat::Wired;
    out->crc_checked = true;
    return true;
}

bool frame_from_bytes(const uint8_t* in, size_t len, Frame* out) {
    if (len < 10) return false;
    if (parse_wired(in, len, out)) return true;
    // Plain frame without CRCs: L+1 == len. A format B frame with CRCs has the
    // same length relation, so check its CRC first.
    if ((size_t)in[0] + 1 == len && len <= sizeof(out->data)) {
        frame_reset(out, LinkMode::Unknown);
        if (frame_trim_format_b(in, len, out)) return true;
        frame_reset(out, LinkMode::Unknown);
        memcpy(out->data, in, len);
        out->len = (uint16_t)len;
        return true;
    }
    // Frame with DLL CRCs.
    frame_reset(out, LinkMode::Unknown);
    size_t na = frame_len_format_a(in[0]);
    if (na <= len && frame_trim_format_a(in, na, out)) return true;
    size_t nb = frame_len_format_b(in[0]);
    if (nb <= len && frame_trim_format_b(in, nb, out)) return true;
    // Raw captures as they come from the radio.
    size_t used = 0;
    if (frame_from_capture(in, len, RadioBand::CT, out, &used) == CaptureStatus::Ok) return true;
    // Lenient: a CRC-less frame followed by junk (L+1 < len).
    if ((size_t)in[0] + 1 < len && in[0] >= 10) {
        frame_reset(out, LinkMode::Unknown);
        memcpy(out->data, in, (size_t)in[0] + 1);
        out->len = (uint16_t)(in[0] + 1);
        return true;
    }
    return false;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t hex_to_bytes(const char* hex, uint8_t* out, size_t out_max) {
    size_t n = 0;
    int hi = -1;
    for (const char* p = hex; *p; ++p) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hi < 0) { ++p; continue; }
        int v = hexval(*p);
        if (v < 0) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '|' || *p == '_' || *p == ':' || *p == '-' || *p == '#')
                continue;
            return 0; // invalid character
        }
        if (hi < 0) { hi = v; continue; }
        if (n >= out_max) return 0;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    // Like upstream hex2bin a dangling last nibble is ignored.
    return n;
}

void bytes_to_hex(const uint8_t* in, size_t len, char* out, size_t out_max, bool upper) {
    static const char* HU = "0123456789ABCDEF";
    static const char* HL = "0123456789abcdef";
    const char* H = upper ? HU : HL;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < out_max; ++i) {
        out[o++] = H[in[i] >> 4];
        out[o++] = H[in[i] & 0xF];
    }
    if (out_max) out[o < out_max ? o : out_max - 1] = 0;
}

} // namespace wmb
