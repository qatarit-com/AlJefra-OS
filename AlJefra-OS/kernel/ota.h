/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Over-The-Air (OTA) Update Interface
 *
 * Provides a complete OTA update pipeline:
 *   1. Check the AlJefra marketplace for available updates
 *   2. Download the update package to a staging buffer
 *   3. Verify the Ed25519 signature
 *   4. Apply the update to the kernel storage area
 *
 * The update package is a signed binary with the same Ed25519
 * key used for .ajdrv driver packages.  The staging buffer is a
 * static 256 KB region (no malloc).
 *
 * Rollback: if the newly applied kernel fails to boot, the
 * bootloader should detect the rollback flag and restore the
 * previous kernel image.
 */

#ifndef ALJEFRA_OTA_H
#define ALJEFRA_OTA_H

#include "../hal/hal.h"

/* OTA update state machine */
typedef enum {
    OTA_NONE        = 0,    /* No update activity */
    OTA_AVAILABLE   = 1,    /* Update found on marketplace */
    OTA_DOWNLOADING = 2,    /* Download in progress */
    OTA_VERIFYING   = 3,    /* Signature verification in progress */
    OTA_READY       = 4,    /* Verified, ready to apply */
    OTA_APPLYING    = 5,    /* Writing to storage */
    OTA_DONE        = 6,    /* Successfully applied */
    OTA_ERROR       = 7,    /* Error at some stage */
} ota_status_t;

/* Update metadata returned by ota_check() */
typedef struct {
    char     version[32];       /* New version string (e.g. "1.0.1") */
    char     url[256];          /* Download URL */
    uint32_t size;              /* Expected package size in bytes */
    uint32_t crc32;             /* Expected CRC32 of the package */
} ota_update_info_t;

/* Maximum staging buffer size (256 KB, no heap allocation) */
#define OTA_STAGING_SIZE    (256 * 1024)

/* Check the marketplace for available updates.
 * current_version: our running OS version string (e.g. "1.0.0").
 * On success (update available), fills *info and returns HAL_OK.
 * Returns HAL_ERROR if no update, HAL_TIMEOUT on network failure. */
hal_status_t ota_check(const char *current_version, ota_update_info_t *info);

/* Download the update package to the internal staging buffer.
 * Must call ota_check() first.  Reports progress via console.
 * Returns HAL_OK on successful download, HAL_ERROR on failure. */
hal_status_t ota_download(const ota_update_info_t *info);

/* Verify the Ed25519 signature on the staged package.
 * Uses the key from ed25519_key.h / store/verify.c.
 * Returns HAL_OK if signature is valid, HAL_ERROR otherwise. */
hal_status_t ota_verify(void);

/* Apply the verified update to the kernel storage area.
 * Writes the staging buffer to the storage driver, then sets
 * a "pending reboot" flag.
 * Returns HAL_OK on success, HAL_ERROR on write failure. */
hal_status_t ota_apply(void);

/* Run the full OTA pipeline: check -> download -> verify -> apply.
 * Convenience wrapper.  Returns final status. */
hal_status_t ota_run(const char *current_version);

/* Get the current OTA status */
ota_status_t ota_get_status(void);

/* Get human-readable status string */
const char *ota_status_string(ota_status_t status);

#endif /* ALJEFRA_OTA_H */
