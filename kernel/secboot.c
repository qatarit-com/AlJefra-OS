/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Secure Boot Chain Implementation
 *
 * Kernel self-verification using SHA-512 hashing.  At boot, computes the
 * hash of the kernel's code + data sections and compares it against the
 * expected hash stored in the .secboot linker section (patched post-build
 * by tools/sign_kernel.py).
 *
 * Driver packages are verified via the existing Ed25519 path in
 * store/verify.c, wrapped here with policy enforcement.
 */

#include "secboot.h"
#include "klog.h"
#include "ed25519_key.h"
#include "../hal/hal.h"
#include "../store/verify.h"

/* ---- Linker-provided symbols ---- */

extern uint8_t _kernel_start[];   /* Start of kernel image       */
extern uint8_t _secboot_start[];  /* Start of .secboot section   */
extern uint8_t _secboot_end[];    /* End of .secboot section     */

/* On x86_64 the symbol is _bss_start; on ARM64/RISC-V it's __bss_start.
 * We define weak aliases so the linker picks whichever is available. */
extern uint8_t _bss_start[]   __attribute__((weak));
extern uint8_t __bss_start[]  __attribute__((weak));

/* ---- Embedded hash (placed in .secboot section) ---- */

/* This array lives in the .secboot linker section.  At compile time it is
 * all zeros.  After building, tools/sign_kernel.py patches these 64 bytes
 * with the actual SHA-512 hash of the kernel binary. */
static const uint8_t secboot_expected_hash[SECBOOT_HASH_SIZE]
    __attribute__((section(".secboot"), used)) = {0};

/* ---- Module state ---- */

static secboot_policy_t g_policy = SECBOOT_AUDIT;
static int g_initialized;

/* ---- Helpers ---- */

/* Check if the embedded hash is all zeros (unsigned / dev build). */
static int hash_is_zero(const uint8_t *h)
{
    for (int i = 0; i < SECBOOT_HASH_SIZE; i++) {
        if (h[i] != 0) return 0;
    }
    return 1;
}

/* Compare two hashes.  Returns 0 if equal. */
static int hash_compare(const uint8_t *a, const uint8_t *b)
{
    int diff = 0;
    for (int i = 0; i < SECBOOT_HASH_SIZE; i++)
        diff |= a[i] ^ b[i];
    return diff;
}

/* Format a hash as hex for logging (first 16 bytes → 32 hex chars). */
static void hash_to_hex(const uint8_t *h, char *buf, int nbytes)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < nbytes; i++) {
        buf[i * 2]     = hex[h[i] >> 4];
        buf[i * 2 + 1] = hex[h[i] & 0x0F];
    }
    buf[nbytes * 2] = '\0';
}

/* Halt the system (used in ENFORCE mode on failure). */
static void secboot_halt(const char *reason)
{
    hal_console_puts("[secboot] FATAL: ");
    hal_console_puts(reason);
    hal_console_puts("\n[secboot] System halted.\n");
    klog(KLOG_FATAL, "[secboot] %s — halted", reason);
    klog_flush();
    for (;;)
        hal_cpu_halt();
}

/* ---- Public API ---- */

void secboot_init(void)
{
    if (g_initialized)
        return;

    g_initialized = 1;
    g_policy = SECBOOT_AUDIT;

    klog(KLOG_INFO, "[secboot] Secure boot initialized (policy: AUDIT)");
    hal_console_puts("[secboot] Secure boot initialized (policy: AUDIT)\n");
}

void secboot_verify_self(void)
{
    if (!g_initialized) {
        secboot_init();
    }

    if (g_policy == SECBOOT_DISABLED) {
        klog(KLOG_DEBUG, "[secboot] Verification disabled, skipping");
        return;
    }

    /* Check if we have a signed build (non-zero embedded hash) */
    if (hash_is_zero(secboot_expected_hash)) {
        const char *msg = "Kernel hash is zeros (unsigned build)";
        klog(KLOG_WARN, "[secboot] %s", msg);
        hal_console_puts("[secboot] WARN: ");
        hal_console_puts(msg);
        hal_console_puts("\n");

        if (g_policy == SECBOOT_ENFORCE) {
            secboot_halt("Unsigned kernel in ENFORCE mode");
        }
        return;
    }

    /* Determine the region to hash: from _kernel_start to the start
     * of the .secboot section.  This covers .text, .rodata, and any
     * sections placed before .secboot in the linker script.
     *
     * We deliberately exclude .secboot itself (which contains the
     * expected hash) and .bss (which is zeroed at runtime). */
    const uint8_t *region_start = _kernel_start;
    const uint8_t *region_end   = _secboot_start;
    uint64_t region_size = (uint64_t)(region_end - region_start);

    if (region_size == 0 || region_end <= region_start) {
        klog(KLOG_ERROR, "[secboot] Invalid kernel region (start=%p end=%p)",
             region_start, region_end);
        if (g_policy == SECBOOT_ENFORCE)
            secboot_halt("Invalid kernel memory layout");
        return;
    }

    /* Compute SHA-512 of the kernel region */
    uint8_t computed_hash[64];
    sha512(region_start, region_size, computed_hash);

    /* Format hashes for logging */
    char expected_hex[33], computed_hex[33];
    hash_to_hex(secboot_expected_hash, expected_hex, 16);
    hash_to_hex(computed_hash, computed_hex, 16);

    /* Compare */
    if (hash_compare(computed_hash, secboot_expected_hash) == 0) {
        klog(KLOG_INFO, "[secboot] Kernel integrity VERIFIED (hash: %s...)",
             computed_hex);
        hal_console_puts("[secboot] Kernel integrity VERIFIED\n");
    } else {
        klog(KLOG_ERROR, "[secboot] Kernel hash MISMATCH!");
        klog(KLOG_ERROR, "[secboot]   expected: %s...", expected_hex);
        klog(KLOG_ERROR, "[secboot]   computed: %s...", computed_hex);

        hal_console_puts("[secboot] ERROR: Kernel hash MISMATCH\n");
        hal_console_puts("[secboot]   expected: ");
        hal_console_puts(expected_hex);
        hal_console_puts("...\n");
        hal_console_puts("[secboot]   computed: ");
        hal_console_puts(computed_hex);
        hal_console_puts("...\n");

        if (g_policy == SECBOOT_ENFORCE) {
            secboot_halt("Kernel integrity check failed");
        }
    }
}

int secboot_check_driver(const void *data, uint64_t size)
{
    if (g_policy == SECBOOT_DISABLED)
        return 0;

    hal_status_t rc = ajdrv_verify(data, size);
    if (rc == HAL_OK)
        return 0;

    klog(KLOG_ERROR, "[secboot] Driver signature verification FAILED");

    if (g_policy == SECBOOT_ENFORCE) {
        hal_console_puts("[secboot] ENFORCE: Rejecting unsigned driver\n");
        return -1;
    }

    /* AUDIT mode: warn but allow */
    hal_console_puts("[secboot] AUDIT: Loading driver despite failed verification\n");
    return -1;
}

secboot_policy_t secboot_get_policy(void)
{
    return g_policy;
}

void secboot_set_policy(secboot_policy_t policy)
{
    g_policy = policy;

    const char *name;
    switch (policy) {
    case SECBOOT_DISABLED: name = "DISABLED"; break;
    case SECBOOT_AUDIT:    name = "AUDIT";    break;
    case SECBOOT_ENFORCE:  name = "ENFORCE";  break;
    default:               name = "UNKNOWN";  break;
    }

    klog(KLOG_INFO, "[secboot] Policy set to %s", name);
}
