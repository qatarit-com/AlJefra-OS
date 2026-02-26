/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Package Signature Verification
 *
 * Implements Ed25519 signature verification for .ajdrv driver packages.
 *
 * Ed25519 is used because:
 *   - 32-byte keys, 64-byte signatures (small)
 *   - Fast verification (~70K verifications/sec on modern CPUs)
 *   - No NIST curves, no patents
 *   - Deterministic signatures (no RNG needed for signing)
 *
 * This is a MINIMAL Ed25519 verification implementation suitable for
 * bare-metal use.  For the full implementation, we'd use a dedicated
 * Ed25519 library.  Here we provide the framework and SHA-512 needed
 * for the verification path.
 */

#include "verify.h"
#include "package.h"
#include "../hal/hal.h"

/* ── Trusted public key (set at build time or by marketplace) ── */
static uint8_t g_trusted_key[AJDRV_PUBKEY_SIZE];
static int g_key_set;

void ajdrv_set_trusted_key(const uint8_t pub_key[AJDRV_PUBKEY_SIZE])
{
    for (int i = 0; i < AJDRV_PUBKEY_SIZE; i++)
        g_trusted_key[i] = pub_key[i];
    g_key_set = 1;
}

/* ── SHA-512 (needed for Ed25519 verification) ── */

typedef struct {
    uint64_t state[8];
    uint64_t count;
    uint8_t  buffer[128];
    uint32_t buf_len;
} sha512_ctx_t;

static const uint64_t sha512_k[80] = {
    0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL,
    0xE9B5DBA58189DBBCULL, 0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL,
    0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL, 0xD807AA98A3030242ULL,
    0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
    0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL,
    0xC19BF174CF692694ULL, 0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL,
    0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL, 0x2DE92C6F592B0275ULL,
    0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
    0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL,
    0xBF597FC7BEEF0EE4ULL, 0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL,
    0x06CA6351E003826FULL, 0x142929670A0E6E70ULL, 0x27B70A8546D22FFCULL,
    0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
    0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL,
    0x92722C851482353BULL, 0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL,
    0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL, 0xD192E819D6EF5218ULL,
    0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
    0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL,
    0x34B0BCB5E19B48A8ULL, 0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL,
    0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL, 0x748F82EE5DEFB2FCULL,
    0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
    0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL,
    0xC67178F2E372532BULL, 0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL,
    0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL, 0x06F067AA72176FBAULL,
    0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
    0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL,
    0x431D67C49C100D4CULL, 0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL,
    0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL,
};

static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_compress(sha512_ctx_t *ctx, const uint8_t block[128])
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++)
            w[i] = (w[i] << 8) | block[i * 8 + j];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i-15], 1) ^ rotr64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = rotr64(w[i-2], 19) ^ rotr64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint64_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint64_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t temp1 = h + S1 + ch + sha512_k[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha512_init(sha512_ctx_t *ctx)
{
    ctx->state[0] = 0x6A09E667F3BCC908ULL;
    ctx->state[1] = 0xBB67AE8584CAA73BULL;
    ctx->state[2] = 0x3C6EF372FE94F82BULL;
    ctx->state[3] = 0xA54FF53A5F1D36F1ULL;
    ctx->state[4] = 0x510E527FADE682D1ULL;
    ctx->state[5] = 0x9B05688C2B3E6C1FULL;
    ctx->state[6] = 0x1F83D9ABFB41BD6BULL;
    ctx->state[7] = 0x5BE0CD19137E2179ULL;
    ctx->count = 0;
    ctx->buf_len = 0;
}

static void sha512_update(sha512_ctx_t *ctx, const uint8_t *data, uint64_t len)
{
    while (len > 0) {
        uint32_t space = 128 - ctx->buf_len;
        uint32_t chunk = (len < space) ? (uint32_t)len : space;
        for (uint32_t i = 0; i < chunk; i++)
            ctx->buffer[ctx->buf_len + i] = data[i];
        ctx->buf_len += chunk;
        data += chunk;
        len -= chunk;
        ctx->count += chunk;

        if (ctx->buf_len == 128) {
            sha512_compress(ctx, ctx->buffer);
            ctx->buf_len = 0;
        }
    }
}

static void sha512_final(sha512_ctx_t *ctx, uint8_t hash[64])
{
    uint64_t bits = ctx->count * 8;

    /* Padding */
    ctx->buffer[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 112) {
        while (ctx->buf_len < 128) ctx->buffer[ctx->buf_len++] = 0;
        sha512_compress(ctx, ctx->buffer);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 112) ctx->buffer[ctx->buf_len++] = 0;

    /* Length in big-endian (128-bit, but we only use lower 64 bits) */
    for (int i = 0; i < 8; i++)
        ctx->buffer[112 + i] = 0;
    for (int i = 0; i < 8; i++)
        ctx->buffer[120 + i] = (uint8_t)(bits >> (56 - i * 8));

    sha512_compress(ctx, ctx->buffer);

    /* Output */
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            hash[i * 8 + j] = (uint8_t)(ctx->state[i] >> (56 - j * 8));
}

/* ── Ed25519 verification ── */
/*
 * Full Ed25519 verification requires:
 *   1. SHA-512 hash of (R || pub_key || message) → h
 *   2. Scalar multiplication on Curve25519
 *   3. Point decompression and comparison
 *
 * This is a placeholder that validates the package structure
 * and computes SHA-512 for integrity.  A full Ed25519 implementation
 * (e.g., from SUPERCOP or TweetNaCl) should be integrated for
 * production use.
 */

hal_status_t ajdrv_verify_signature(const uint8_t pub_key[AJDRV_PUBKEY_SIZE],
                                     const void *data, uint64_t size)
{
    if (size < sizeof(ajdrv_header_t) + AJDRV_SIGNATURE_SIZE)
        return HAL_ERROR;

    const ajdrv_header_t *hdr = (const ajdrv_header_t *)data;

    /* Validate header */
    int rc = ajdrv_validate_header(hdr, size);
    if (rc != 0) {
        hal_console_printf("[verify] Invalid header: %d\n", rc);
        return HAL_ERROR;
    }

    /* The signed region is [0..signature_offset) */
    uint32_t signed_len = hdr->signature_offset;
    const uint8_t *signature = (const uint8_t *)data + hdr->signature_offset;

    /* Compute SHA-512 over the signed region for integrity check */
    sha512_ctx_t sha;
    uint8_t hash[64];
    sha512_init(&sha);
    sha512_update(&sha, (const uint8_t *)data, signed_len);
    sha512_final(&sha, hash);

    hal_console_printf("[verify] SHA-512 of %u signed bytes computed\n", signed_len);

    /* TODO: Full Ed25519 signature verification using:
     *   R = signature[0..31]  (curve point)
     *   S = signature[32..63] (scalar)
     *   h = SHA-512(R || pub_key || message)
     *   Verify: [S]B == R + [h]A  (where B = base point, A = pub_key point)
     *
     * For production, integrate TweetNaCl's crypto_sign_open() or
     * a minimal Ed25519 verify implementation (~500 lines of C).
     */

    /* Placeholder: accept if header is valid and hash computed successfully */
    hal_console_puts("[verify] WARNING: Ed25519 not yet implemented, accepting package\n");

    (void)pub_key;
    (void)signature;

    return HAL_OK;
}

hal_status_t ajdrv_verify(const void *data, uint64_t size)
{
    if (!g_key_set) {
        hal_console_puts("[verify] No trusted key set\n");
        return HAL_ERROR;
    }
    return ajdrv_verify_signature(g_trusted_key, data, size);
}
