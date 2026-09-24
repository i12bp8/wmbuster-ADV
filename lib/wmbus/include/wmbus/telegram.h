// wM-Buster ADV — wM-Bus telegram layers: DLL, ELL, NWL, AFL and TPL
// parsing with decryption (ELL AES-CTR, TPL AES-CBC modes 5 and 7 incl. the
// CMAC key derivation, AES-CCM mode 10, DES modes 2 and 3, Diehl LFSR "real
// data"). Port of the wmbusmeters Telegram::parse* logic onto fixed size
// buffers.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "wmbus/frame.h"

namespace wmb {

enum class DecryptStatus : uint8_t {
    NotEncrypted = 0,  // plain telegram
    Decrypted,         // decrypted and verified (2F2F / CRC / MAC)
    NoKey,             // encrypted and no key available
    WrongKey,          // decryption verification failed
    Unsupported,       // encryption mode not supported (8, 9, ...)
};
const char* decrypt_status_name(DecryptStatus s);

// Security modes from the TPL configuration field (bits 8..12).
enum TplSecMode : uint8_t {
    TPL_SEC_NONE = 0,
    TPL_SEC_MFCT = 1,
    TPL_SEC_DES_NO_IV = 2,
    TPL_SEC_DES_IV = 3,
    TPL_SEC_AES_CBC_IV = 5,     // "mode 5"
    TPL_SEC_AES_CBC_NO_IV = 7,  // "mode 7"
    TPL_SEC_AES_CTR_CMAC = 8,
    TPL_SEC_AES_CGM = 9,
    TPL_SEC_AES_CCM = 10,
    TPL_SEC_SPECIFIC_16_31 = 16,
};

struct TelegramOptions {
    const uint8_t* key;            // meter key (16 byte AES, or 8 byte DES), or nullptr
    uint8_t key_len;               // 16 (also when 0) or 8
    const uint8_t (*default_keys)[16]; // fallback keys (driver provided), may be nullptr
    uint8_t num_default_keys;
    bool permit_sanxing_609b;      // driver transform_payload=buggy_sanxing_609B
    uint32_t now_unix;             // current time (DES mode 3 IV), 0 = unknown
    bool simulated;                // replayed/pasted telegram: content may already be decrypted
};

struct Telegram {
    // Working copy of the frame (decryption happens in place). For wireless
    // frames buf[0] is the L-field. For wired M-Bus buf holds [C A CI ...].
    uint8_t  buf[WMB_FRAME_MAX];
    uint16_t len;
    LinkMode mode;
    bool     wired;

    // Original first 10 bytes before a Diehl address transform (for LFSR).
    uint8_t  original[10];
    bool     has_original;

    // DLL
    uint8_t  dll_c;
    uint16_t dll_mfct;
    uint8_t  dll_mfct_b[2];
    uint8_t  dll_a[6];         // id (4, LSB first) + version + type
    uint8_t  dll_version;
    uint8_t  dll_type;
    char     dll_id[9];        // printable id, e.g. "12345678"
    uint8_t  mbus_primary_address;

    // ELL
    uint8_t  ell_ci;
    uint8_t  ell_cc;
    uint8_t  ell_acc;
    bool     ell_has_sn;
    uint8_t  ell_sn_b[4];
    uint8_t  ell_sec;          // 0 none, 1 AES-CTR
    bool     ell_has_address;
    uint16_t ell_mfct;
    uint8_t  ell_a[6];

    // AFL
    uint8_t  afl_ci;
    uint16_t afl_fc;
    uint8_t  afl_mcl;
    bool     afl_has_counter;
    uint8_t  afl_counter_b[4];
    uint8_t  afl_mac[16];
    uint8_t  afl_mac_len;

    // TPL
    uint8_t  tpl_ci;
    uint16_t tpl_start;        // offset of the TPL CI field in buf
    bool     tpl_id_found;
    uint8_t  tpl_a[6];         // id (4) + version + type
    uint8_t  tpl_mfct_b[2];
    uint16_t tpl_mfct;
    uint8_t  tpl_version;
    uint8_t  tpl_type;
    uint8_t  tpl_acc;
    uint8_t  tpl_sts;
    int16_t  tpl_sts_offset;   // -1 when not present
    uint16_t tpl_cfg;
    uint8_t  tpl_sec_mode;
    uint8_t  tpl_num_encr_blocks;
    uint8_t  tpl_cfg_ext;
    uint8_t  tpl_kdf_selection;
    bool     tpl_counter_found;  // mode 10 message counter
    uint8_t  tpl_counter_b[4];
    uint8_t  tpl_ccm_tag_size;
    uint16_t tpl_aad_len;        // mode 10: bytes from tpl_start authenticated as aad
    bool     tpl_ccm_tag_ok;
    uint16_t format_signature; // compact frames (CI 73/79/7B)
    bool     compact;

    // Result of parsing
    uint16_t header_size;      // offset of the application payload in buf
    uint16_t suffix_size;      // bytes at the end that are not payload
    bool     mfct_specific;    // payload is manufacturer specific (CI A0..B7 etc.)
    bool     header_ok;        // DLL (+ELL/AFL/TPL headers) parsed
    bool     ci_unknown;       // unsupported CI field
    DecryptStatus decrypt;
    uint8_t  used_key[16];     // key that decrypted the telegram (if any)
    bool     used_default_key;
    // Problems found while decoding (FAILED_DECODE, MISSING_KEY, ...); joined
    // into the driver status field like upstream.
    char     decoding_errors[64];

    // Convenience (after parse)
    const uint8_t* payload() const { return buf + header_size; }
    size_t payload_len() const { return len > header_size + suffix_size ? len - header_size - suffix_size : 0; }

    // Manufacturer / version / type used for driver detection: TPL when a long
    // TPL header is present, else DLL.
    uint16_t mvt_mfct() const { return tpl_id_found ? tpl_mfct : dll_mfct; }
    uint8_t  mvt_version() const { return tpl_id_found ? tpl_version : dll_version; }
    uint8_t  mvt_type() const { return tpl_id_found ? tpl_type : dll_type; }
    // The meter id as printed by wmbusmeters (TPL id if present, else DLL id).
    void     meter_id(char out[9]) const;
};

// Parse a frame. Always fills the DLL part if possible; returns false when
// the frame is too short to be a telegram. Decryption results are reported in
// t->decrypt (the payload is only meaningful for NotEncrypted/Decrypted).
bool telegram_parse(const Frame& f, const TelegramOptions& opt, Telegram* t);

// Diehl (IZAR/PRIOS/Sharky) specifics -------------------------------------
enum class DiehlFrame : uint8_t { NA, OMS, PRIOS, PRIOS_SCR, SAP_PRIOS, SAP_PRIOS_STD, REAL_DATA, RESERVED };
DiehlFrame diehl_frame_interpretation(const uint8_t* frame, size_t len);
// LFSR decoding (IZAR PRIOS payloads). Returns decoded length or 0.
size_t diehl_lfsr_decode(const uint8_t* origin, const uint8_t* frame, size_t frame_len, uint32_t key,
                         bool check_header_4b, uint8_t check_value, uint8_t* out);
uint32_t diehl_convert_key(const uint8_t* bytes8);
// Default PRIOS keys (key1, key2) as 32-bit LFSR seeds.
void diehl_default_keys(uint32_t out[2]);

} // namespace wmb
