/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Package Signature Verification */

#ifndef ALJEFRA_VERIFY_H
#define ALJEFRA_VERIFY_H

#include <stdint.h>
#include "../hal/hal.h"
#include "package.h"

/* Verify the Ed25519 signature on an .ajdrv package.
 * pub_key: 32-byte Ed25519 public key
 * data: pointer to the entire .ajdrv file
 * size: total file size
 * Returns HAL_OK if signature is valid.
 */
hal_status_t ajdrv_verify_signature(const uint8_t pub_key[AJDRV_PUBKEY_SIZE],
                                     const void *data, uint64_t size);

/* Set the trusted public key for the AlJefra Store */
void ajdrv_set_trusted_key(const uint8_t pub_key[AJDRV_PUBKEY_SIZE]);

/* Verify a package using the trusted key */
hal_status_t ajdrv_verify(const void *data, uint64_t size);

/* Compute SHA-512 hash of arbitrary data.
 * out: must point to a 64-byte buffer. */
void sha512(const uint8_t *data, uint64_t len, uint8_t hash[64]);

#endif /* ALJEFRA_VERIFY_H */
