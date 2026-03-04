/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Secure Boot Chain
 *
 * Provides kernel self-verification at boot time using SHA-512 hashing
 * and Ed25519 signature verification.  The kernel binary contains a
 * .secboot section with an expected SHA-512 hash that is patched
 * post-build by tools/sign_kernel.py.
 *
 * Boot flow:
 *   1. secboot_init()        — set policy (ENFORCE / AUDIT / DISABLED)
 *   2. secboot_verify_self() — hash kernel sections, compare to embedded hash
 *   3. secboot_check_driver()— verify .ajdrv packages per policy
 *
 * Policies:
 *   ENFORCE  — halt on verification failure (production)
 *   AUDIT    — log warning and continue (default)
 *   DISABLED — skip all verification
 */

#ifndef ALJEFRA_SECBOOT_H
#define ALJEFRA_SECBOOT_H

#include <stdint.h>

/* ---- Signed kernel image format (.ajkrn) ---- */

/* Magic: "AJKN" in little-endian */
#define AJKRN_MAGIC      0x4E4B4A41

/* Current format version */
#define AJKRN_VERSION    1

/* .ajkrn file header (128 bytes, prepended to raw kernel binary) */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* 0x00: AJKRN_MAGIC                      */
    uint32_t version;            /* 0x04: AJKRN_VERSION                    */
    uint32_t kernel_size;        /* 0x08: Size of kernel binary (bytes)    */
    uint32_t flags;              /* 0x0C: Reserved flags                   */
    uint8_t  sha512_hash[64];    /* 0x10: SHA-512 of the raw kernel binary */
    uint8_t  signature[64];      /* 0x50: Ed25519 signature over [0..0x50) */
} ajkrn_header_t;                /* Total: 128 bytes (0x80)                */

/* ---- Secure boot policy ---- */

typedef enum {
    SECBOOT_DISABLED = 0,  /* Skip all verification                     */
    SECBOOT_AUDIT    = 1,  /* Log warnings but continue on failure      */
    SECBOOT_ENFORCE  = 2,  /* Halt on any verification failure          */
} secboot_policy_t;

/* ---- Embedded kernel hash (lives in .secboot linker section) ---- */

/* Size of the SHA-512 hash embedded in the kernel binary.
 * Initialized to all zeros at compile time; patched by sign_kernel.py. */
#define SECBOOT_HASH_SIZE  64

/* ---- Public API ---- */

/* Initialize the secure boot subsystem.
 * Sets the verification policy (defaults to SECBOOT_AUDIT).
 * Must be called before secboot_verify_self(). */
void secboot_init(void);

/* Verify the kernel's own integrity.
 * Computes SHA-512 of the kernel .text + .rodata + .data sections
 * and compares against the hash embedded in the .secboot section.
 * Action on failure depends on the current policy. */
void secboot_verify_self(void);

/* Verify a driver package (.ajdrv) according to the current policy.
 * Returns 0 on success (or if policy is DISABLED).
 * Returns -1 on verification failure (AUDIT: logs and returns -1,
 * ENFORCE: halts before returning). */
int secboot_check_driver(const void *data, uint64_t size);

/* Get the current secure boot policy. */
secboot_policy_t secboot_get_policy(void);

/* Set the secure boot policy. */
void secboot_set_policy(secboot_policy_t policy);

#endif /* ALJEFRA_SECBOOT_H */
