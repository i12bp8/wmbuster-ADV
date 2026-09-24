// wM-Buster ADV — wM-Bus data link frames (EN 13757-4).
// Converts raw radio captures (after the sync word) into CRC-checked frames,
// and accepts user supplied hex (with or without DLL CRCs, or wired M-Bus
// long frames) for the analyzer.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

#define WMB_FRAME_MAX 300   // L-field (255 max) + 1, plus room for wired M-Bus framing

enum class LinkMode : uint8_t { Unknown = 0, C1, T1, S1, MBus };
enum class FrameFormat : uint8_t { None = 0, A, B, Wired };

const char* link_mode_name(LinkMode m);

struct Frame {
    uint8_t     data[WMB_FRAME_MAX]; // wireless: [L C M M A A A A A A CI ...], CRCs removed, L = len-1
    uint16_t    len;                 // wired: [C A CI ...] (no 68 L L 68 header, no CS/16 trailer)
    LinkMode    mode;
    FrameFormat format;
    bool        crc_checked;         // false when the input had no DLL CRCs (analyzer)
    int16_t     rssi;                // dBm (radio only)
};

// Radio band the capture was taken on.
enum class RadioBand : uint8_t { CT = 0, S = 1 };

enum class CaptureStatus : uint8_t {
    Ok = 0,
    TooShort,       // not even a header
    BadCoding,      // invalid 3-out-of-6 / Manchester symbol
    Truncated,      // L-field announces more bytes than were captured
    CrcError,       // DLL CRC mismatch
    BadLength,      // L-field too small to be a telegram
};
const char* capture_status_name(CaptureStatus s);

// On-air length (including DLL CRCs) of a frame format A/B frame with L-field l.
size_t frame_len_format_a(uint8_t l);
inline size_t frame_len_format_b(uint8_t l) { return (size_t)l + 1; }

// Strip DLL CRCs from an L-field-first buffer. Output L is rewritten to len-1.
bool frame_trim_format_a(const uint8_t* in, size_t in_len, Frame* out);
bool frame_trim_format_b(const uint8_t* in, size_t in_len, Frame* out);

// Decode a raw capture (bytes following the 16-bit sync word) into a frame.
// For the C/T band: C-mode captures start with 54 CD (format A) or 54 3D
// (format B); everything else is treated as 3-out-of-6 coded T-mode.
// For the S band the capture is Manchester coded. *consumed receives the number
// of raw bytes the frame occupied on air (for statistics / diagnostics).
CaptureStatus frame_from_capture(const uint8_t* raw, size_t raw_len, RadioBand band,
                                 Frame* out, size_t* consumed);

// Interpret arbitrary bytes supplied by a user (analyzer, serial, web):
// a CRC-less frame (L+1 == len), frame format A or B with CRCs, a wired M-Bus
// long frame (68 L L 68 ... CS 16), or a raw radio capture (54 CD..., T1 3of6).
bool frame_from_bytes(const uint8_t* in, size_t len, Frame* out);

// Hex helpers. Parse ignores whitespace, '|', '_', ':', '-', '#', an optional
// 0x prefix and a dangling last nibble (like upstream hex2bin).
size_t hex_to_bytes(const char* hex, uint8_t* out, size_t out_max);
void   bytes_to_hex(const uint8_t* in, size_t len, char* out, size_t out_max, bool upper = true);

} // namespace wmb
