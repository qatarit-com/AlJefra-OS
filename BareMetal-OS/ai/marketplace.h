/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Driver Marketplace Client
 *
 * Communicates with api.aljefra.com to:
 *   1. Send hardware manifests
 *   2. Receive driver recommendations
 *   3. Download signed .ajdrv driver packages
 */

#ifndef ALJEFRA_MARKETPLACE_H
#define ALJEFRA_MARKETPLACE_H

#include <stdint.h>
#include "../hal/hal.h"
#include "../kernel/ai_bootstrap.h"

/* Marketplace API endpoints */
#define MARKETPLACE_HOST   "api.aljefra.com"
#define MARKETPLACE_PORT   443

/* API paths */
#define API_CATALOG        "/v1/catalog"
#define API_MANIFEST       "/v1/manifest"
#define API_DRIVER         "/v1/drivers"
#define API_UPDATES        "/v1/updates"

/* Driver recommendation from marketplace */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    char     driver_name[64];
    char     version[16];
    uint32_t size_bytes;
    char     sha256[65];    /* Hex-encoded SHA-256 */
    char     download_url[256];
} marketplace_driver_info_t;

/* Connect to the marketplace (TLS handshake) */
hal_status_t marketplace_connect(void);

/* Disconnect */
void marketplace_disconnect(void);

/* Send hardware manifest, receive driver recommendations.
 * Returns list of recommended drivers. */
hal_status_t marketplace_send_manifest(const hardware_manifest_t *manifest);

/* Download a specific driver package.
 * Allocates memory for the data; caller must free via hal_dma_free. */
hal_status_t marketplace_get_driver(uint16_t vendor_id, uint16_t device_id,
                                     void **data, uint64_t *size);

/* Check for OS updates */
hal_status_t marketplace_check_updates(const char *os_version,
                                        char *update_url, uint32_t url_max);

/* Get the driver catalog (list of all available drivers) */
hal_status_t marketplace_get_catalog(marketplace_driver_info_t *drivers,
                                      uint32_t max, uint32_t *count);

#endif /* ALJEFRA_MARKETPLACE_H */
