// wM-Buster ADV — AES-128 primitives for wM-Bus security (EN 13757-7 / OMS):
// block encrypt/decrypt, CBC decrypt, CTR keystream and AES-CMAC (used by the
// mode 7/10 key derivation and MAC checks), plus DES-CBC for modes 2 and 3.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

struct Aes128 {
    uint8_t rk[176]; // expanded round keys
};

void aes128_init(Aes128* ctx, const uint8_t key[16]);
void aes128_encrypt_block(const Aes128* ctx, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt_block(const Aes128* ctx, const uint8_t in[16], uint8_t out[16]);

// CBC decrypt len bytes (multiple of 16). in and out may alias.
void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                        const uint8_t* in, uint8_t* out, size_t len);

// CTR mode as used by the ELL (counter block incremented big-endian per block).
void aes128_ctr_crypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, uint8_t* out, size_t len);

// AES-CMAC (RFC 4493).
void aes128_cmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16]);

// DES-CBC decrypt (FIPS 46-3) for the deprecated TPL security modes 2 and 3.
// len must be a multiple of 8. in and out may alias.
bool des_cbc_decrypt(const uint8_t key[8], const uint8_t iv[8], const uint8_t* in, uint8_t* out, size_t len);

} // namespace wmb
