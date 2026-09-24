// wM-Buster ADV — wM-Bus telegram layers (DLL/ELL/NWL/AFL/TPL) + security.
// GPL-3.0
#include "wmbus/telegram.h"
#include "wmbus/aes.h"
#include "wmbus/crc.h"
#include "wmbus/types.h"

#include <stdio.h>
#include <string.h>

namespace wmb {

const char* decrypt_status_name(DecryptStatus s) {
    switch (s) {
    case DecryptStatus::NotEncrypted: return "plain";
    case DecryptStatus::Decrypted: return "decrypted";
    case DecryptStatus::NoKey: return "encrypted (no key)";
    case DecryptStatus::WrongKey: return "encrypted (wrong key)";
    case DecryptStatus::Unsupported: return "encrypted (unsupported)";
    }
    return "?";
}

static const uint8_t* aes_key(const TelegramOptions& opt) {
    return opt.key && (opt.key_len == 0 || opt.key_len == 16) ? opt.key : nullptr;
}

static void add_decoding_error(Telegram* t, const char* err) {
    size_t n = strlen(t->decoding_errors);
    if (strstr(t->decoding_errors, err)) return;
    snprintf(t->decoding_errors + n, sizeof(t->decoding_errors) - n, "%s%s", n ? " " : "", err);
}

void Telegram::meter_id(char out[9]) const {
    if (tpl_id_found) {
        snprintf(out, 9, "%02x%02x%02x%02x", tpl_a[3], tpl_a[2], tpl_a[1], tpl_a[0]);
    } else {
        memcpy(out, dll_id, 9);
    }
}

// ---------------------------------------------------------------------------
// Diehl specifics (manufacturer_specificities.cc)
// ---------------------------------------------------------------------------
static bool is_diehl(uint16_t m) {
    static const char* const D[] = { "DME", "EWT", "HYD", "SAP", "SPL" };
    for (const char* d : D) if (mfct_from_str(d) == (m & 0x7fff)) return true;
    return false;
}

DiehlFrame diehl_frame_interpretation(const uint8_t* frame, size_t len) {
    if (len < 15) return DiehlFrame::NA;
    uint8_t c = frame[1];
    uint16_t m = (uint16_t)(frame[3] << 8 | frame[2]);
    uint8_t ci = frame[10];
    uint16_t cfg = (uint16_t)(frame[14] << 8 | frame[13]);
    if (!is_diehl(m)) return DiehlFrame::NA;
    if (c != 0x44 && c != 0x46) return DiehlFrame::NA;
    bool sap = (m & 0x7fff) == mfct_from_str("SAP");
    switch (ci) {
    case 0x71: return DiehlFrame::REAL_DATA;
    case 0x7A:
        if (((cfg >> 8) & 0x10) == 0x10) return DiehlFrame::REAL_DATA;
        return DiehlFrame::OMS;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        return sap ? DiehlFrame::SAP_PRIOS : DiehlFrame::PRIOS;
    case 0xB0:
        return sap ? DiehlFrame::SAP_PRIOS_STD : DiehlFrame::RESERVED;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        return DiehlFrame::RESERVED;
    case 0xB1: case 0xB2: case 0xB3:
        return DiehlFrame::PRIOS_SCR;
    default:
        return DiehlFrame::OMS;
    }
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t diehl_convert_key(const uint8_t* b) { return be32(b) ^ be32(b + 4); }

void diehl_default_keys(uint32_t out[2]) {
    static const uint8_t K1[8] = { 0x39, 0xBC, 0x8A, 0x10, 0xE6, 0x6D, 0x83, 0xF8 };
    static const uint8_t K2[8] = { 0x51, 0x72, 0x89, 0x10, 0xE6, 0x6D, 0x83, 0xF8 };
    out[0] = diehl_convert_key(K1);
    out[1] = diehl_convert_key(K2);
}

size_t diehl_lfsr_decode(const uint8_t* origin, const uint8_t* frame, size_t frame_len, uint32_t key,
                         bool check_header_4b, uint8_t check_value, uint8_t* out) {
    if (frame_len < 15) return 0;
    key ^= be32(origin + 2);
    key ^= be32(origin + 6);
    key ^= be32(frame + 10);
    size_t size = frame_len - 15;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; ++i) {
        for (int j = 0; j < 8; ++j) {
            uint8_t bit = (uint8_t)(((key & 0x2) != 0) ^ ((key & 0x4) != 0) ^ ((key & 0x800) != 0) ^ ((key & 0x80000000u) != 0));
            key = (key << 1) | bit;
        }
        out[i] = (uint8_t)(frame[i + 15] ^ (key & 0xFF));
        if (check_header_4b && out[0] != 0x4B) return 0;
        checksum += out[i];
    }
    if (!check_header_4b) {
        if ((checksum & 0xEF) != check_value) return 0;
    }
    return size;
}

static void diehl_preprocess(Telegram* t) {
    DiehlFrame fi = diehl_frame_interpretation(t->buf, t->len);
    if (fi == DiehlFrame::PRIOS || fi == DiehlFrame::PRIOS_SCR || fi == DiehlFrame::REAL_DATA) {
        memcpy(t->original, t->buf, 10);
        t->has_original = true;
        uint8_t version = t->buf[4], type = t->buf[5];
        for (int i = 4; i < 8; ++i) t->buf[i] = t->buf[i + 2];
        t->buf[8] = version;
        t->buf[9] = type;
    } else if (fi == DiehlFrame::SAP_PRIOS) {
        memcpy(t->original, t->buf, 10);
        t->has_original = true;
        t->buf[8] = 0x00;
        t->buf[9] = 0x07;
    }
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------
struct Cursor {
    Telegram* t;
    size_t pos;
    size_t remaining() const { return t->len > pos ? t->len - pos : 0; }
    bool has(size_t n) const { return remaining() >= n; }
    uint8_t at(size_t i) const { return t->buf[pos + i]; }
};

static bool parse_dll(Cursor& c) {
    Telegram* t = c.t;
    if (!c.has(11)) return false;
    // buf[0] is the L-field
    t->dll_c = t->buf[1];
    t->dll_mfct_b[0] = t->buf[2];
    t->dll_mfct_b[1] = t->buf[3];
    t->dll_mfct = (uint16_t)(t->buf[3] << 8 | t->buf[2]);
    for (int i = 0; i < 6; ++i) t->dll_a[i] = t->buf[4 + i];
    t->dll_version = t->buf[8];
    t->dll_type = t->buf[9];
    snprintf(t->dll_id, sizeof(t->dll_id), "%02x%02x%02x%02x", t->buf[7], t->buf[6], t->buf[5], t->buf[4]);
    c.pos = 10;
    return true;
}

static bool is_ell(uint8_t ci) { return ci == 0x8C || ci == 0x8D || ci == 0x8E || ci == 0x8F || ci == 0x86; }

// Returns false only on a hard parse error.
static bool parse_ell(Cursor& c, const TelegramOptions& opt) {
    Telegram* t = c.t;
    if (!c.has(1)) return false;
    uint8_t ci = c.at(0);
    if (!is_ell(ci)) return true;
    if (ci == 0x86) return false; // ELL V not handled (as upstream)
    t->ell_ci = ci;
    c.pos++;
    bool has_addr = (ci == 0x8E || ci == 0x8F);
    bool has_sn = (ci == 0x8D || ci == 0x8F);
    if (!c.has(2)) return false;
    t->ell_cc = c.at(0);
    t->ell_acc = c.at(1);
    c.pos += 2;
    if (has_addr) {
        if (!c.has(8)) return false;
        t->ell_has_address = true;
        t->ell_mfct = (uint16_t)(c.at(1) << 8 | c.at(0));
        for (int i = 0; i < 6; ++i) t->ell_a[i] = c.at(2 + i);
        c.pos += 8;
    }
    if (has_sn) {
        if (!c.has(4 + 2)) return false;
        t->ell_has_sn = true;
        for (int i = 0; i < 4; ++i) t->ell_sn_b[i] = c.at(i);
        c.pos += 4;
        uint32_t sn = (uint32_t)t->ell_sn_b[3] << 24 | (uint32_t)t->ell_sn_b[2] << 16 |
                      (uint32_t)t->ell_sn_b[1] << 8 | t->ell_sn_b[0];
        t->ell_sec = (uint8_t)((sn >> 29) & 0x7);
        uint8_t* enc = t->buf + c.pos;            // payload CRC + payload
        size_t enc_len = t->len - c.pos;
        const uint8_t* key = aes_key(opt);
        bool have_key = key != nullptr;
        if (t->ell_sec == 1 && !have_key) {
            // Without a key the payload might already be decrypted (replayed
            // logs): accept it when the payload CRC checks out, like upstream.
            uint16_t pl_crc0 = (uint16_t)(enc[1] << 8 | enc[0]);
            if (enc_len >= 2 && pl_crc0 == crc16_en13757(enc + 2, enc_len - 2)) {
                c.pos += 2;
                return true;
            }
            t->decrypt = DecryptStatus::NoKey;
            return true;
        }
        if (t->ell_sec == 1) {
            uint8_t iv[16];
            int i = 0;
            iv[i++] = t->dll_mfct_b[0];
            iv[i++] = t->dll_mfct_b[1];
            for (int j = 0; j < 6; ++j) iv[i++] = t->dll_a[j];
            iv[i++] = (uint8_t)(t->ell_cc & ~0x10 & ~0x02);
            for (int j = 0; j < 4; ++j) iv[i++] = t->ell_sn_b[j];
            iv[i++] = 0; iv[i++] = 0; iv[i++] = 0;
            aes128_ctr_crypt(key, iv, enc, enc, enc_len);
            memcpy(t->used_key, key, 16);
        } else if (t->ell_sec != 0) {
            t->decrypt = DecryptStatus::Unsupported;
            return true;
        }
        uint16_t pl_crc = (uint16_t)(enc[1] << 8 | enc[0]);
        uint16_t check = crc16_en13757(enc + 2, enc_len - 2);
        c.pos += 2;
        if (pl_crc != check) {
            t->decrypt = DecryptStatus::WrongKey;
            return true;
        }
        if (t->ell_sec == 1) t->decrypt = DecryptStatus::Decrypted;
    }
    return true;
}

static bool parse_nwl(Cursor& c) {
    if (!c.has(1)) return false;
    if (c.at(0) != 0x81) return true;
    if (!c.has(2)) return false;
    c.pos += 2;
    return true;
}

static bool parse_afl(Cursor& c) {
    Telegram* t = c.t;
    if (!c.has(1)) return false;
    if (c.at(0) != 0x90) return true;
    t->afl_ci = 0x90;
    c.pos++;
    if (!c.has(1)) return false;
    c.pos++; // afl length
    if (!c.has(2)) return false;
    t->afl_fc = (uint16_t)(c.at(1) << 8 | c.at(0));
    c.pos += 2;
    bool has_key_info = t->afl_fc & 0x0200;
    bool has_mac = t->afl_fc & 0x0400;
    bool has_counter = t->afl_fc & 0x0800;
    bool has_control = t->afl_fc & 0x2000;
    if (has_control) {
        if (!c.has(1)) return false;
        t->afl_mcl = c.at(0);
        c.pos++;
    }
    if (has_key_info) {
        if (!c.has(2)) return false;
        c.pos += 2;
    }
    if (has_counter) {
        if (!c.has(4)) return false;
        for (int i = 0; i < 4; ++i) t->afl_counter_b[i] = c.at(i);
        t->afl_has_counter = true;
        c.pos += 4;
    }
    if (has_mac) {
        static const uint8_t LEN[9] = { 0, 0, 0, 2, 4, 8, 12, 16, 12 };
        int at = t->afl_mcl & 0x0f;
        int len = at < 9 ? LEN[at] : 0;
        if (len == 0) return false;
        if (!c.has((size_t)len)) return false;
        for (int i = 0; i < len; ++i) t->afl_mac[i] = c.at(i);
        t->afl_mac_len = (uint8_t)len;
        c.pos += len;
    }
    return true;
}

static bool parse_tpl_config(Cursor& c, const TelegramOptions& opt) {
    Telegram* t = c.t;
    if (!c.has(2)) return false;
    t->tpl_cfg = (uint16_t)(c.at(1) << 8 | c.at(0));
    c.pos += 2;
    uint8_t m = (uint8_t)((t->tpl_cfg >> 8) & 0x1f);
    t->tpl_sec_mode = m >= 16 ? (uint8_t)TPL_SEC_SPECIFIC_16_31 : m;
    bool has_cfg_ext = false;
    if (m == TPL_SEC_AES_CBC_IV) t->tpl_num_encr_blocks = (uint8_t)((t->tpl_cfg >> 4) & 0x0f);
    if (m == TPL_SEC_AES_CBC_NO_IV) {
        t->tpl_num_encr_blocks = (uint8_t)((t->tpl_cfg >> 4) & 0x0f);
        has_cfg_ext = true;
    }
    if (has_cfg_ext) {
        if (!c.has(1)) return false;
        t->tpl_cfg_ext = c.at(0);
        t->tpl_kdf_selection = (uint8_t)((t->tpl_cfg_ext >> 4) & 3);
        c.pos++;
    }
    if (m == TPL_SEC_AES_CCM) {
        // OMS security profile D: cfg extension (tag size, kdf, key id), an
        // optional key version byte, then a 4 byte message counter. The tpl
        // header up to the counter is the additional authenticated data.
        if (!c.has(2)) return false;
        uint16_t cfe = (uint16_t)(c.at(1) << 8 | c.at(0));
        c.pos += 2;
        t->tpl_ccm_tag_size = (uint8_t)(4 + ((cfe >> 8) & 0x03) * 4);
        t->tpl_kdf_selection = (uint8_t)((cfe >> 4) & 0x03);
        if ((cfe >> 6) & 1) { if (!c.has(1)) return false; c.pos++; }
        t->tpl_aad_len = (uint16_t)(c.pos - t->tpl_start);
        if (!c.has(4)) return false;
        for (int i = 0; i < 4; ++i) t->tpl_counter_b[i] = c.at(i);
        t->tpl_counter_found = true;
        c.pos += 4;
    }
    (void)opt;
    return true;
}

static bool parse_short_tpl(Cursor& c, const TelegramOptions& opt) {
    Telegram* t = c.t;
    if (!c.has(2)) return false;
    t->tpl_acc = c.at(0);
    t->tpl_sts = c.at(1);
    t->tpl_sts_offset = (int16_t)(c.pos + 1);
    c.pos += 2;
    return parse_tpl_config(c, opt);
}

static bool parse_long_tpl(Cursor& c, const TelegramOptions& opt) {
    Telegram* t = c.t;
    if (!c.has(8)) return false;
    t->tpl_id_found = true;
    for (int i = 0; i < 4; ++i) t->tpl_a[i] = c.at(i);
    t->tpl_mfct_b[0] = c.at(4);
    t->tpl_mfct_b[1] = c.at(5);
    t->tpl_mfct = (uint16_t)(c.at(5) << 8 | c.at(4));
    t->tpl_version = c.at(6);
    t->tpl_type = c.at(7);
    t->tpl_a[4] = t->tpl_version;
    t->tpl_a[5] = t->tpl_type;
    c.pos += 8;
    return parse_short_tpl(c, opt);
}

// Mode 5: AES-CBC with IV = M + A + ACC*8. Decrypts in place.
static bool decrypt_mode5(Telegram* t, size_t pos, const uint8_t* key) {
    size_t avail = t->len - pos;
    size_t n = avail;
    if (t->tpl_num_encr_blocks) n = (size_t)t->tpl_num_encr_blocks * 16;
    if (n > avail) {
        n = avail;
        if (n < 16) return false;
    }
    n -= n % 16;
    if (n < 16) return false;
    uint8_t iv[16];
    int i = 0;
    if (t->tpl_id_found) {
        iv[i++] = t->tpl_mfct_b[0]; iv[i++] = t->tpl_mfct_b[1];
        for (int j = 0; j < 6; ++j) iv[i++] = t->tpl_a[j];
    } else {
        iv[i++] = t->dll_mfct_b[0]; iv[i++] = t->dll_mfct_b[1];
        for (int j = 0; j < 6; ++j) iv[i++] = t->dll_a[j];
    }
    for (int j = 0; j < 8; ++j) iv[i++] = t->tpl_acc;
    aes128_cbc_decrypt(key, iv, t->buf + pos, t->buf + pos, n);
    return true;
}

// Key derivation (KDF-A): Kenc = CMAC(K, 00 || counter || id || 07*7). The
// counter is the TPL message counter when present (mode 10), else the AFL one.
static bool derive_kenc(const Telegram* t, const uint8_t* key, uint8_t kenc[16]) {
    if (t->tpl_kdf_selection != 1) return false;
    uint8_t input[16];
    input[0] = 0x00;
    memcpy(input + 1, t->tpl_counter_found ? t->tpl_counter_b : t->afl_counter_b, 4);
    if (t->tpl_id_found) memcpy(input + 5, t->tpl_a, 4);
    else memcpy(input + 5, t->dll_a, 4);
    for (int i = 9; i < 16; ++i) input[i] = 0x07;
    aes128_cmac(key, input, 16, kenc);
    return true;
}

static bool decrypt_mode7(Telegram* t, size_t pos, const uint8_t* key) {
    uint8_t kenc[16];
    if (!derive_kenc(t, key, kenc)) return false;
    size_t avail = t->len - pos;
    size_t n = avail;
    if (t->tpl_num_encr_blocks) n = (size_t)t->tpl_num_encr_blocks * 16;
    if (n > avail) n = avail;
    n -= n % 16;
    if (n < 16) return false;
    uint8_t iv[16] = { 0 };
    aes128_cbc_decrypt(kenc, iv, t->buf + pos, t->buf + pos, n);
    return true;
}

static bool check_2f2f(const Telegram* t, size_t pos) {
    return pos + 2 <= t->len && t->buf[pos] == 0x2F && t->buf[pos + 1] == 0x2F;
}

// Mode 10: AES-CCM (RFC 3610, L=2) with the derived Kenc. The tag is a suffix
// after the application data and is checked over the aad and the plaintext.
static bool decrypt_mode10(Telegram* t, size_t pos, const uint8_t* kenc) {
    size_t avail = t->len - pos;
    size_t tag_size = t->tpl_ccm_tag_size;
    if (avail < tag_size) return false;
    size_t n = avail - tag_size;
    int ncfg = t->tpl_cfg & 0xff;  // 0xff = everything encrypted
    if (ncfg != 0xff && (size_t)ncfg < n) n = (size_t)ncfg;

    const uint8_t* mfct_b = t->tpl_id_found ? t->tpl_mfct_b : t->dll_mfct_b;
    const uint8_t* id_b = t->tpl_id_found ? t->tpl_a : t->dll_a;
    uint8_t nonce[13];
    nonce[0] = mfct_b[0];
    nonce[1] = mfct_b[1];
    memcpy(nonce + 2, id_b, 4);
    nonce[6] = t->tpl_id_found ? t->tpl_version : t->dll_version;
    nonce[7] = t->tpl_id_found ? t->tpl_type : t->dll_type;
    nonce[8] = 0x00;
    for (int i = 0; i < 4; ++i) nonce[9 + i] = t->tpl_counter_b[3 - i];  // counter enters big endian

    Aes128 ctx;
    aes128_init(&ctx, kenc);
    uint8_t a[16], ks[16];
    a[0] = 0x01;
    memcpy(a + 1, nonce, 13);
    for (size_t b = 0; b * 16 < n; ++b) {
        a[14] = (uint8_t)((b + 1) >> 8);
        a[15] = (uint8_t)(b + 1);
        aes128_encrypt_block(&ctx, a, ks);
        size_t m = n - b * 16 < 16 ? n - b * 16 : 16;
        for (size_t i = 0; i < m; ++i) t->buf[pos + b * 16 + i] ^= ks[i];
    }
    t->suffix_size = (uint16_t)tag_size;

    // CBC-MAC over B0 || aad length || aad || plaintext, zero padded.
    uint8_t x[16] = { 0 }, blk[16];
    size_t fill = 0;
    auto feed = [&](uint8_t byte) {
        blk[fill++] = byte;
        if (fill == 16) {
            for (int i = 0; i < 16; ++i) x[i] ^= blk[i];
            aes128_encrypt_block(&ctx, x, x);
            fill = 0;
        }
    };
    auto pad = [&]() {
        if (!fill) return;
        while (fill) feed(0);
    };
    feed((uint8_t)(0x40 | (((tag_size - 2) / 2) << 3) | 0x01));
    for (int i = 0; i < 13; ++i) feed(nonce[i]);
    feed((uint8_t)(n >> 8));
    feed((uint8_t)n);
    feed((uint8_t)(t->tpl_aad_len >> 8));
    feed((uint8_t)t->tpl_aad_len);
    for (size_t i = 0; i < t->tpl_aad_len; ++i) feed(t->buf[t->tpl_start + i]);
    pad();
    for (size_t i = 0; i < n; ++i) feed(t->buf[pos + i]);
    pad();
    a[14] = 0;
    a[15] = 0;
    aes128_encrypt_block(&ctx, a, ks);  // S0 hides the tag
    bool ok = true;
    for (size_t i = 0; i < tag_size; ++i) ok &= (uint8_t)(x[i] ^ ks[i]) == t->buf[t->len - tag_size + i];
    t->tpl_ccm_tag_ok = ok;
    return true;
}

// Modes 2 and 3: DES-CBC over all (whole 8 byte blocks of) remaining bytes.
static bool decrypt_des(Telegram* t, size_t pos, const uint8_t* key8, const uint8_t iv[8]) {
    size_t n = (t->len - pos) / 8 * 8;
    if (n == 0) return false;
    return des_cbc_decrypt(key8, iv, t->buf + pos, t->buf + pos, n);
}

// Decrypts the TPL payload at pos when needed. Sets t->decrypt. Returns the
// number of verification bytes (2F2F) that belong to the header.
static size_t tpl_decrypt(Telegram* t, size_t pos, const TelegramOptions& opt) {
    uint8_t mode = t->tpl_sec_mode;
    if (mode == TPL_SEC_NONE) return 0;

    if (mode == TPL_SEC_AES_CBC_IV || mode == TPL_SEC_AES_CBC_NO_IV) {
        if (check_2f2f(t, pos)) {
            // Already decrypted (replayed log telegram).
            t->decrypt = DecryptStatus::NotEncrypted;
            return 2;
        }
        // Candidate keys: meter key, then driver defaults, then Diehl OMS default.
        const uint8_t* cands[8];
        bool is_default[8];
        int nc = 0;
        const uint8_t* key = aes_key(opt);
        if (key) { cands[nc] = key; is_default[nc++] = false; }
        for (int i = 0; i < opt.num_default_keys && nc < 6; ++i) { cands[nc] = opt.default_keys[i]; is_default[nc++] = true; }
        static const uint8_t DIEHL_DEFAULT[16] = { 0x51, 0x72, 0x89, 0x10, 0xE6, 0x6D, 0x83, 0xF8,
                                                   0x51, 0x72, 0x89, 0x10, 0xE6, 0x6D, 0x83, 0xF8 };
        if (!key && mode == TPL_SEC_AES_CBC_IV && diehl_frame_interpretation(t->buf, t->len) == DiehlFrame::OMS) {
            cands[nc] = DIEHL_DEFAULT;
            is_default[nc++] = true;
        }
        if (nc == 0) {
            t->decrypt = DecryptStatus::NoKey;
            return 0;
        }
        uint8_t saved[WMB_FRAME_MAX];
        size_t n = t->len - pos;
        memcpy(saved, t->buf + pos, n);
        for (int k = 0; k < nc; ++k) {
            bool ok = mode == TPL_SEC_AES_CBC_IV ? decrypt_mode5(t, pos, cands[k]) : decrypt_mode7(t, pos, cands[k]);
            bool good = ok && check_2f2f(t, pos);
            if (!good && ok && mode == TPL_SEC_AES_CBC_IV && opt.permit_sanxing_609b) {
                good = pos + 2 <= t->len && t->buf[pos] == 0x60 && t->buf[pos + 1] == 0x9b &&
                       t->buf[t->len - 2] == 0x2f && t->buf[t->len - 1] == 0x2f;
            }
            if (good) {
                t->decrypt = DecryptStatus::Decrypted;
                memcpy(t->used_key, cands[k], 16);
                t->used_default_key = is_default[k];
                return 2;
            }
            memcpy(t->buf + pos, saved, n);
        }
        t->decrypt = key ? DecryptStatus::WrongKey : DecryptStatus::NoKey;
        return 0;
    }

    if (mode == TPL_SEC_AES_CCM) {
        const uint8_t* key = aes_key(opt);
        uint8_t kenc[16];
        if (!key) {
            t->decrypt = DecryptStatus::NoKey;
            return 0;
        }
        if (!derive_kenc(t, key, kenc)) {
            t->decrypt = DecryptStatus::Unsupported;
            return 0;
        }
        if (!decrypt_mode10(t, pos, kenc)) {
            t->decrypt = DecryptStatus::WrongKey;
            return 0;
        }
        // No 2F2F check bytes here, the tag protects the content. Like
        // upstream a bad tag is reported but the content is still decoded.
        t->decrypt = DecryptStatus::Decrypted;
        memcpy(t->used_key, key, 16);
        if (!t->tpl_ccm_tag_ok) add_decoding_error(t, "FAILED_DECODE");
        return 0;
    }

    if (mode == TPL_SEC_DES_NO_IV || mode == TPL_SEC_DES_IV) {
        if (check_2f2f(t, pos)) {
            t->decrypt = DecryptStatus::NotEncrypted;
            return 2;
        }
        if (!opt.key) {
            t->decrypt = DecryptStatus::NoKey;
            return 0;
        }
        uint8_t saved[WMB_FRAME_MAX];
        size_t n = t->len - pos;
        memcpy(saved, t->buf + pos, n);
        // Mode 3: IV = id(4) + mfct(2) + transmission date (type G); try
        // today and the two days before to cover clock drift.
        int tries = mode == TPL_SEC_DES_IV ? 3 : 1;
        if (mode == TPL_SEC_DES_IV && opt.now_unix == 0) tries = 0;
        for (int k = 0; k < tries; ++k) {
            uint8_t iv[8] = { 0 };
            if (mode == TPL_SEC_DES_IV) {
                long long day = (long long)opt.now_unix / 86400 - k;
                // civil date from days since epoch
                long long z = day + 719468, era = z / 146097, doe = z - era * 146097;
                long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
                int d = (int)(doy - (153 * mp + 2) / 5 + 1), mo = (int)(mp < 10 ? mp + 3 : mp - 9);
                int y = (int)(yoe + era * 400 + (mo <= 2)) - 2000;
                memcpy(iv, t->dll_a, 4);
                iv[4] = t->dll_mfct_b[0];
                iv[5] = t->dll_mfct_b[1];
                iv[6] = (uint8_t)(((mo & 0x07) << 5) | (d & 0x1F));
                iv[7] = (uint8_t)(((y & 0x7F) << 1) | ((mo >> 3) & 0x01));
            }
            if (decrypt_des(t, pos, opt.key, iv) && check_2f2f(t, pos)) {
                t->decrypt = DecryptStatus::Decrypted;
                memset(t->used_key, 0, sizeof(t->used_key));
                memcpy(t->used_key, opt.key, 8);
                return 2;
            }
            memcpy(t->buf + pos, saved, n);
        }
        t->decrypt = DecryptStatus::WrongKey;
        return 0;
    }

    if (mode == TPL_SEC_SPECIFIC_16_31) {
        if (diehl_frame_interpretation(t->buf, t->len) != DiehlFrame::REAL_DATA) return 0;
        uint32_t keys[3];
        int nk = 0;
        if (opt.key) keys[nk++] = diehl_convert_key(opt.key);
        if (nk == 0) { diehl_default_keys(keys); nk = 2; }
        const uint8_t* origin = t->has_original ? t->original : t->buf;
        uint8_t out[WMB_FRAME_MAX];
        for (int k = 0; k < nk; ++k) {
            size_t n = diehl_lfsr_decode(origin, t->buf, t->len, keys[k], false, (uint8_t)(t->buf[14] & 0xEF), out);
            if (n) {
                memcpy(t->buf + 15, out, n);
                t->decrypt = DecryptStatus::Decrypted;
                return 0;
            }
        }
        // A replayed telegram (log, analyzer) may already be decrypted.
        if (opt.simulated) return 0;
        t->decrypt = opt.key ? DecryptStatus::WrongKey : DecryptStatus::NoKey;
        return 0;
    }

    if (mode == TPL_SEC_MFCT) return 0; // manufacturer specific, leave to the driver
    t->decrypt = DecryptStatus::Unsupported;
    return 0;
}

static bool parse_tpl(Cursor& c, const TelegramOptions& opt) {
    Telegram* t = c.t;
    if (!c.has(1)) return false;
    uint8_t ci = c.at(0);
    bool mfct_specific = ci >= 0xA0 && ci <= 0xB7;
    bool known = ci == 0x51 || ci == 0x72 || ci == 0x73 || ci == 0x78 || ci == 0x79 || ci == 0x7A || ci == 0x7B;
    if (!known && !mfct_specific) {
        t->ci_unknown = true;
        t->tpl_ci = ci;
        t->header_size = (uint16_t)(c.pos + 1);
        return false;
    }
    t->tpl_ci = ci;
    t->tpl_start = (uint16_t)c.pos;
    c.pos++;
    switch (ci) {
    case 0x72:
    case 0x73:
        if (!parse_long_tpl(c, opt)) return false;
        break;
    case 0x7A:
    case 0x7B:
        if (!parse_short_tpl(c, opt)) return false;
        break;
    case 0x78:
    case 0x79:
        break;
    default:
        // 0x51 and manufacturer specific CI fields: raw payload.
        t->mfct_specific = true;
        t->header_size = (uint16_t)c.pos;
        return true;
    }
    if (ci == 0x72 || ci == 0x73 || ci == 0x7A || ci == 0x7B) c.pos += tpl_decrypt(t, c.pos, opt);
    if (ci == 0x73 || ci == 0x79 || ci == 0x7B) {
        if (t->decrypt == DecryptStatus::NoKey || t->decrypt == DecryptStatus::WrongKey) {
            t->header_size = (uint16_t)c.pos;
            return true;
        }
        if (!c.has(4)) return false;
        t->format_signature = (uint16_t)(c.at(1) << 8 | c.at(0));
        t->compact = true;
        c.pos += 4; // signature + data crc
    }
    t->header_size = (uint16_t)c.pos;
    return true;
}

static bool parse_wired(Cursor& c, const TelegramOptions& opt) {
    Telegram* t = c.t;
    // buf = [C A CI ...]
    if (t->len == 1 && t->buf[0] == 0xE5) return false;
    if (!c.has(3)) return false;
    t->dll_c = t->buf[0];
    t->mbus_primary_address = t->buf[1];
    snprintf(t->dll_id, sizeof(t->dll_id), "p%d", t->mbus_primary_address);
    c.pos = 2;
    uint8_t ci = c.at(0);
    if (ci != 0x72) {
        t->ci_unknown = true;
        t->tpl_ci = ci;
        return false;
    }
    t->tpl_ci = ci;
    t->tpl_start = (uint16_t)c.pos;
    c.pos++;
    if (!parse_long_tpl(c, opt)) return false;
    // Use the TPL address as the DLL one so that the rest of the code works.
    t->dll_mfct = t->tpl_mfct;
    t->dll_mfct_b[0] = t->tpl_mfct_b[0];
    t->dll_mfct_b[1] = t->tpl_mfct_b[1];
    memcpy(t->dll_a, t->tpl_a, 6);
    t->dll_version = t->tpl_version;
    t->dll_type = t->tpl_type;
    c.pos += tpl_decrypt(t, c.pos, opt);
    t->header_size = (uint16_t)c.pos;
    return true;
}

bool telegram_parse(const Frame& f, const TelegramOptions& opt, Telegram* t) {
    memset(t, 0, offsetof(Telegram, buf));
    memset(&t->len, 0, sizeof(Telegram) - offsetof(Telegram, len));
    t->tpl_sts_offset = -1;
    if (f.len > sizeof(t->buf)) return false;
    memcpy(t->buf, f.data, f.len);
    t->len = f.len;
    t->mode = f.mode;
    t->wired = f.format == FrameFormat::Wired;
    t->decrypt = DecryptStatus::NotEncrypted;

    Cursor c{ t, 0 };
    if (t->wired) {
        t->header_ok = parse_wired(c, opt);
        return t->len >= 3;
    }
    diehl_preprocess(t);
    if (!parse_dll(c)) return false;
    if (!parse_ell(c, opt)) { t->header_size = (uint16_t)c.pos; return true; }
    if (t->decrypt == DecryptStatus::NoKey || t->decrypt == DecryptStatus::WrongKey ||
        t->decrypt == DecryptStatus::Unsupported) {
        t->header_size = (uint16_t)c.pos;
        t->header_ok = true;
        return true;
    }
    if (!parse_nwl(c)) { t->header_size = (uint16_t)c.pos; return true; }
    if (!parse_afl(c)) { t->header_size = (uint16_t)c.pos; return true; }
    t->header_ok = parse_tpl(c, opt);
    if (!t->header_ok && !t->header_size) t->header_size = (uint16_t)c.pos;
    return true;
}

} // namespace wmb
