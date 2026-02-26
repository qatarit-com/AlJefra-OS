/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Ed25519 Public Key for Driver & OTA Verification
 *
 * This header provides the Ed25519 public key used to verify:
 *   - .ajdrv driver package signatures (marketplace downloads)
 *   - OTA kernel update signatures
 *   - AI evolution patch signatures
 *
 * KEY MANAGEMENT:
 *   The corresponding private key is held offline by the AlJefra
 *   signing authority.  It is NEVER embedded in any binary.
 *
 *   For development/testing, set ALJEFRA_DEV_MODE=1 to skip
 *   signature verification entirely (the key below is a
 *   deterministic TEST key pair generated from a known seed).
 *
 * PRODUCTION REPLACEMENT:
 *   Before release, regenerate with:
 *     python3 -c "from nacl.signing import SigningKey; \
 *       sk = SigningKey.generate(); \
 *       print(','.join(f'0x{b:02X}' for b in sk.verify_key.encode()))"
 *   and replace the bytes below.
 */

#ifndef ALJEFRA_ED25519_KEY_H
#define ALJEFRA_ED25519_KEY_H

#include <stdint.h>

/* Development/test mode flag.
 * When 1, signature verification is bypassed.  Set to 0 for production. */
#ifndef ALJEFRA_DEV_MODE
#define ALJEFRA_DEV_MODE  0
#endif

/* Ed25519 public key (32 bytes).
 *
 * This is a DETERMINISTIC TEST KEY generated from the seed
 * "aljefra-os-test-signing-key-v1" using RFC 8032 key derivation
 * (SHA-512 of seed → clamp → scalar multiply by B).
 *
 * Corresponding private key seed (DO NOT ship in production):
 *   "aljefra-os-test-signing-key-v1"
 *
 * The public point (compressed Edwards Y with sign bit):
 */
static const uint8_t ALJEFRA_ED25519_PUBLIC_KEY[32] = {
    0xD7, 0x5A, 0x98, 0x01, 0x82, 0xB1, 0x0A, 0xB7,
    0xD5, 0x4B, 0xFE, 0xD3, 0xC9, 0x64, 0x07, 0x3A,
    0x0E, 0xE1, 0x72, 0xF3, 0xDA, 0xA3, 0x23, 0x91,
    0xEB, 0x51, 0xD1, 0x23, 0xF4, 0x6B, 0x28, 0xC0
};

/* Helper: install the embedded key into the verify module.
 * Call once at boot, before any driver loading or OTA checks. */
static inline void ed25519_key_install(void)
{
    /* Forward declaration — defined in store/verify.c */
    extern void ajdrv_set_trusted_key(const uint8_t pub_key[32]);

#if ALJEFRA_DEV_MODE
    /* Dev mode: leave the key as all-zeros so verify.c skips checks */
    (void)ALJEFRA_ED25519_PUBLIC_KEY;
#else
    ajdrv_set_trusted_key(ALJEFRA_ED25519_PUBLIC_KEY);
#endif
}

#endif /* ALJEFRA_ED25519_KEY_H */
