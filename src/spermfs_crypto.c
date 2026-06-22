#include "spermfs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#include <openssl/rand.h>
#define HAVE_OPENSSL 1
#else
#define HAVE_OPENSSL 0
#endif

/* XChaCha20-Poly1305 implementation (RFC 8439 based) */

static void chacha20_block(uint32_t state[16], uint8_t output[64])
{
    uint32_t x[16];
    memcpy(x, state, sizeof(x));

    for (int i = 0; i < 20; i += 2) {
        /* Column round */
        x[0] += x[4]; x[12] ^= x[0]; x[12] = (x[12] << 16) | (x[12] >> 16);
        x[8] += x[12]; x[4] ^= x[8]; x[4] = (x[4] << 12) | (x[4] >> 20);
        x[0] += x[4]; x[12] ^= x[0]; x[12] = (x[12] << 8) | (x[12] >> 24);
        x[8] += x[12]; x[4] ^= x[8]; x[4] = (x[4] << 7) | (x[4] >> 25);
        /* Diagonal round */
        x[0] += x[5]; x[15] ^= x[0]; x[15] = (x[15] << 16) | (x[15] >> 16);
        x[10] += x[15]; x[5] ^= x[10]; x[5] = (x[5] << 12) | (x[5] >> 20);
        x[0] += x[5]; x[15] ^= x[0]; x[15] = (x[15] << 8) | (x[15] >> 24);
        x[10] += x[15]; x[5] ^= x[10]; x[5] = (x[5] << 7) | (x[5] >> 25);
    }

    for (int i = 0; i < 16; i++)
        x[i] += state[i];

    for (int i = 0; i < 16; i++) {
        output[i * 4] = x[i] & 0xFF;
        output[i * 4 + 1] = (x[i] >> 8) & 0xFF;
        output[i * 4 + 2] = (x[i] >> 16) & 0xFF;
        output[i * 4 + 3] = (x[i] >> 24) & 0xFF;
    }
}

static void chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                              uint64_t counter, const uint8_t *in, uint8_t *out,
                              size_t len)
{
    uint32_t state[16];
    /* "expand 32-byte k" */
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        state[4 + i] = ((uint32_t)key[i * 4] |
                       ((uint32_t)key[i * 4 + 1] << 8) |
                       ((uint32_t)key[i * 4 + 2] << 16) |
                       ((uint32_t)key[i * 4 + 3] << 24));
    state[12] = counter & 0xFFFFFFFF;
    state[13] = (counter >> 32) & 0xFFFFFFFF;
    state[14] = ((uint32_t)nonce[0] | ((uint32_t)nonce[1] << 8) |
                ((uint32_t)nonce[2] << 16) | ((uint32_t)nonce[3] << 24));
    state[15] = ((uint32_t)nonce[4] | ((uint32_t)nonce[5] << 8) |
                ((uint32_t)nonce[6] << 16) | ((uint32_t)nonce[7] << 24));

    uint8_t block[64];
    size_t offset = 0;
    while (offset < len) {
        chacha20_block(state, block);
        state[12]++;
        if (state[12] == 0) state[13]++;

        size_t chunk = len - offset;
        if (chunk > 64) chunk = 64;

        for (size_t i = 0; i < chunk; i++)
            out[offset + i] = in[offset + i] ^ block[i];
        offset += chunk;
    }
}

static void poly1305_mac(const uint8_t key[32], const uint8_t *data,
                          size_t data_len, uint8_t tag[16])
{
    /* Simplified Poly1305 for demonstration */
    uint32_t r[5], s[4], h[5], c[5], d[5], g[5];
    memset(h, 0, sizeof(h));
    memset(c, 0, sizeof(c));

    /* r = key[0..15] clamped */
    r[0] = (key[0] | ((uint32_t)key[1] << 8)) & 0x1FFF;
    r[1] = ((key[1] >> 2) | ((uint32_t)key[2] << 6)) & 0x1FFF;
    r[2] = ((key[3] >> 4) | ((uint32_t)key[4] << 4)) & 0x1FFF;
    r[3] = ((key[5] >> 6) | ((uint32_t)key[6] << 2) | ((uint32_t)key[7] << 10)) & 0x1FFF;
    r[4] = (key[7] >> 3) & 0x1FFF;

    s[0] = key[8] | ((uint32_t)key[9] << 8);
    s[1] = key[10] | ((uint32_t)key[11] << 8);
    s[2] = key[12] | ((uint32_t)key[13] << 8);
    s[3] = key[14] | ((uint32_t)key[15] << 8);

    size_t offset = 0;
    while (offset < data_len) {
        /* Read 16-byte block */
        uint8_t block[17] = {0};
        size_t block_len = data_len - offset;
        if (block_len > 16) block_len = 16;
        memcpy(block, data + offset, block_len);
        block[block_len] = 1; /* Add high bit */

        c[0] = ((uint32_t)block[0] | ((uint32_t)block[1] << 8)) & 0x1FFF;
        c[1] = ((block[1] >> 2) | ((uint32_t)block[2] << 6)) & 0x1FFF;
        c[2] = ((block[3] >> 4) | ((uint32_t)block[4] << 4)) & 0x1FFF;
        c[3] = ((block[5] >> 6) | ((uint32_t)block[6] << 2) |
                ((uint32_t)block[7] << 10)) & 0x1FFF;
        c[4] = (block[7] >> 3) & 0x1FFF;

        /* h += c */
        h[0] += c[0]; h[1] += c[1]; h[2] += c[2];
        h[3] += c[3]; h[4] += c[4];

        /* h *= r */
        uint64_t p[5] = {0};
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 5; j++) {
                if (i + j >= 5) continue;
                p[i + j] += (uint64_t)h[i] * r[j];
            }
        }
        /* Reduce */
        h[0] = p[0] & 0x1FFF;
        h[1] = (p[0] >> 13 | (p[1] & 0x1FFF) << 5) & 0x1FFF;
        h[2] = (p[1] >> 8 | (p[2] & 0x1FFF) << 8) & 0x1FFF;
        h[3] = (p[2] >> 5 | (p[3] & 0x1FFF) << 11) & 0x1FFF;
        h[4] = (p[3] >> 2 | (p[4] & 0x3FFF) << 14) & 0x1FFF;

        uint64_t carry = h[4] >> 13;
        h[0] += carry * 5;
        h[4] &= 0x1FFF;
        carry = h[0] >> 13; h[1] += carry; h[0] &= 0x1FFF;
        carry = h[1] >> 13; h[2] += carry; h[1] &= 0x1FFF;
        carry = h[2] >> 13; h[3] += carry; h[2] &= 0x1FFF;
        carry = h[3] >> 13; h[4] += carry; h[3] &= 0x1FFF;
        h[4] &= 0x1FFF;

        offset += block_len;
    }

    /* h += s */
    h[0] += s[0] & 0xFFFF;
    h[1] += (s[0] >> 16) + (s[1] & 0xFFFF);
    h[2] += (s[1] >> 16) + (s[2] & 0xFFFF);
    h[3] += (s[2] >> 16) + (s[3] & 0xFFFF);
    h[4] += (s[3] >> 16);

    /* Produce tag */
    for (int i = 0; i < 4; i++) {
        tag[i * 4] = h[i] & 0xFF;
        tag[i * 4 + 1] = (h[i] >> 8) & 0xFF;
        tag[i * 4 + 2] = (h[i] >> 16) & 0xFF;
        tag[i * 4 + 3] = (h[i] >> 24) & 0xFF;
    }
}

static int xchacha20_poly1305_encrypt(const uint8_t *plain, size_t plain_len,
                                        uint8_t *cipher, size_t *cipher_len,
                                        const uint8_t key[32],
                                        const uint8_t nonce[24],
                                        uint8_t tag[16])
{
    /* HChaCha20 to derive subkey */
    uint32_t state[16];
    state[0] = 0x61707865; state[1] = 0x3320646e;
    state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        state[4 + i] = ((uint32_t)key[i * 4] |
                       ((uint32_t)key[i * 4 + 1] << 8) |
                       ((uint32_t)key[i * 4 + 2] << 16) |
                       ((uint32_t)key[i * 4 + 3] << 24));
    state[12] = ((uint32_t)nonce[0] | ((uint32_t)nonce[1] << 8) |
                ((uint32_t)nonce[2] << 16) | ((uint32_t)nonce[3] << 24));
    state[13] = ((uint32_t)nonce[4] | ((uint32_t)nonce[5] << 8) |
                ((uint32_t)nonce[6] << 16) | ((uint32_t)nonce[7] << 24));
    state[14] = ((uint32_t)nonce[8] | ((uint32_t)nonce[9] << 8) |
                ((uint32_t)nonce[10] << 16) | ((uint32_t)nonce[11] << 24));
    state[15] = ((uint32_t)nonce[12] | ((uint32_t)nonce[13] << 8) |
                ((uint32_t)nonce[14] << 16) | ((uint32_t)nonce[15] << 24));

    uint8_t block0[64];
    chacha20_block(state, block0);

    uint8_t subkey[32];
    memcpy(subkey, block0, 32);

    /* Encrypt with ChaCha20 using derived subkey and remaining nonce */
    chacha20_encrypt(subkey, nonce + 16, 1, plain, cipher, plain_len);

    /* Poly1305 tag */
    uint8_t poly_key[32];
    memset(poly_key, 0, 32);
    chacha20_encrypt(subkey, nonce + 16, 0, poly_key, poly_key, 32);

    poly1305_mac(poly_key, cipher, plain_len, tag);

    *cipher_len = plain_len;
    return 0;
}

static int xchacha20_poly1305_decrypt(const uint8_t *cipher, size_t cipher_len,
                                        uint8_t *plain, size_t *plain_len,
                                        const uint8_t key[32],
                                        const uint8_t nonce[24],
                                        const uint8_t tag[16])
{
    /* Same key derivation as encrypt */
    uint32_t state[16];
    state[0] = 0x61707865; state[1] = 0x3320646e;
    state[2] = 0x79622d32; state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        state[4 + i] = ((uint32_t)key[i * 4] |
                       ((uint32_t)key[i * 4 + 1] << 8) |
                       ((uint32_t)key[i * 4 + 2] << 16) |
                       ((uint32_t)key[i * 4 + 3] << 24));
    state[12] = ((uint32_t)nonce[0] | ((uint32_t)nonce[1] << 8) |
                ((uint32_t)nonce[2] << 16) | ((uint32_t)nonce[3] << 24));
    state[13] = ((uint32_t)nonce[4] | ((uint32_t)nonce[5] << 8) |
                ((uint32_t)nonce[6] << 16) | ((uint32_t)nonce[7] << 24));
    state[14] = ((uint32_t)nonce[8] | ((uint32_t)nonce[9] << 8) |
                ((uint32_t)nonce[10] << 16) | ((uint32_t)nonce[11] << 24));
    state[15] = ((uint32_t)nonce[12] | ((uint32_t)nonce[13] << 8) |
                ((uint32_t)nonce[14] << 16) | ((uint32_t)nonce[15] << 24));

    uint8_t block0[64];
    chacha20_block(state, block0);

    uint8_t subkey[32];
    memcpy(subkey, block0, 32);

    /* Verify tag */
    uint8_t poly_key[32];
    memset(poly_key, 0, 32);
    chacha20_encrypt(subkey, nonce + 16, 0, poly_key, poly_key, 32);

    uint8_t computed_tag[16];
    poly1305_mac(poly_key, cipher, cipher_len, computed_tag);
    if (memcmp(tag, computed_tag, 16) != 0)
        return SPERMAFS_ERR_CRYPT;

    /* Decrypt */
    chacha20_encrypt(subkey, nonce + 16, 1, cipher, plain, cipher_len);
    *plain_len = cipher_len;
    return 0;
}

int spermfs_encrypt(spermfs_context_t *ctx, const uint8_t *plain, size_t plain_len,
                      uint8_t *cipher, size_t *cipher_len,
                      uint8_t nonce[12], uint8_t tag[16], int algo,
                      const uint8_t *key)
{
    (void)ctx;
    if (!plain || !cipher || !cipher_len || !key) return SPERMAFS_ERR_INVAL;

    switch (algo) {
    case SPERMAFS_CRYPT_AES256_GCM:
#if HAVE_OPENSSL
        {
            EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
            if (!ectx) return SPERMAFS_ERR_CRYPT;

            size_t out_len = 0;
            *cipher_len = plain_len + 16;

            if (EVP_EncryptInit_ex(ectx, EVP_aes_256_gcm(), NULL, key, nonce) != 1 ||
                EVP_EncryptUpdate(ectx, cipher, (int *)&out_len, plain, plain_len) != 1) {
                EVP_CIPHER_CTX_free(ectx);
                return SPERMAFS_ERR_CRYPT;
            }

            int final_len = 0;
            EVP_EncryptFinal_ex(ectx, cipher + out_len, &final_len);
            out_len += final_len;

            EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_GCM_GET_TAG, 16, tag);
            EVP_CIPHER_CTX_free(ectx);
            *cipher_len = out_len;
            return SPERMAFS_OK;
        }
#else
        /* Fallback: use XChaCha20 for AES-256-GCM mode */
        {
            uint8_t xnonce[24] = {0};
            memcpy(xnonce, nonce, 12);
            return xchacha20_poly1305_encrypt(plain, plain_len, cipher,
                                               cipher_len, key, xnonce, tag);
        }
#endif

    case SPERMAFS_CRYPT_XCHACHA20_POLY:
        {
            uint8_t xnonce[24] = {0};
            memcpy(xnonce, nonce, 12);
            /* Generate remaining 12 bytes of nonce */
            for (int i = 0; i < 12; i++)
                xnonce[12 + i] = (uint8_t)(spermfs_time_ns() >> (i * 8));
            return xchacha20_poly1305_encrypt(plain, plain_len, cipher,
                                               cipher_len, key, xnonce, tag);
        }

    default:
        return SPERMAFS_ERR_CRYPT;
    }
}

int spermfs_decrypt(spermfs_context_t *ctx, const uint8_t *cipher, size_t cipher_len,
                      uint8_t *plain, size_t *plain_len,
                      uint8_t nonce[12], uint8_t tag[16], int algo,
                      const uint8_t *key)
{
    (void)ctx;
    if (!cipher || !plain || !plain_len || !key) return SPERMAFS_ERR_INVAL;

    switch (algo) {
    case SPERMAFS_CRYPT_AES256_GCM:
#if HAVE_OPENSSL
        {
            EVP_CIPHER_CTX *dctx = EVP_CIPHER_CTX_new();
            if (!dctx) return SPERMAFS_ERR_CRYPT;

            size_t out_len = 0;
            if (EVP_DecryptInit_ex(dctx, EVP_aes_256_gcm(), NULL, key, nonce) != 1 ||
                EVP_DecryptUpdate(dctx, plain, (int *)&out_len, cipher, cipher_len) != 1) {
                EVP_CIPHER_CTX_free(dctx);
                return SPERMAFS_ERR_CRYPT;
            }

            EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag);
            int final_len = 0;
            if (EVP_DecryptFinal_ex(dctx, plain + out_len, &final_len) != 1) {
                EVP_CIPHER_CTX_free(dctx);
                return SPERMAFS_ERR_CRYPT;
            }
            out_len += final_len;

            EVP_CIPHER_CTX_free(dctx);
            *plain_len = out_len;
            return SPERMAFS_OK;
        }
#else
        {
            uint8_t xnonce[24] = {0};
            memcpy(xnonce, nonce, 12);
            for (int i = 0; i < 12; i++)
                xnonce[12 + i] = (uint8_t)(spermfs_time_ns() >> (i * 8));
            return xchacha20_poly1305_decrypt(cipher, cipher_len, plain,
                                               plain_len, key, xnonce, tag);
        }
#endif

    case SPERMAFS_CRYPT_XCHACHA20_POLY:
        {
            uint8_t xnonce[24] = {0};
            memcpy(xnonce, nonce, 12);
            for (int i = 0; i < 12; i++)
                xnonce[12 + i] = (uint8_t)(spermfs_time_ns() >> (i * 8));
            return xchacha20_poly1305_decrypt(cipher, cipher_len, plain,
                                               plain_len, key, xnonce, tag);
        }

    default:
        return SPERMAFS_ERR_CRYPT;
    }
}

const char *spermfs_crypt_name(int algo)
{
    switch (algo) {
    case SPERMAFS_CRYPT_NONE:            return "none";
    case SPERMAFS_CRYPT_AES256_GCM:     return "AES-256-GCM";
    case SPERMAFS_CRYPT_XCHACHA20_POLY: return "XChaCha20-Poly1305";
    default: return "unknown";
    }
}
