/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Kernel-Level DHCP Client Interface
 *
 * High-level DHCP API used by the kernel bootstrap and OTA subsystem.
 * Wraps the lower-level net/dhcp.c implementation with kernel-specific
 * conveniences: retry logic, configuration caching, and renewal timers.
 *
 * Usage:
 *   dhcp_config_t cfg;
 *   if (dhcp_init(&cfg) == HAL_OK) {
 *       // cfg.ip, cfg.gateway, cfg.dns are valid
 *   }
 */

#ifndef ALJEFRA_KERNEL_DHCP_H
#define ALJEFRA_KERNEL_DHCP_H

#include "../hal/hal.h"

/* Network configuration obtained from DHCP */
typedef struct {
    uint32_t ip;            /* Assigned IP address (host byte order) */
    uint32_t netmask;       /* Subnet mask (host byte order) */
    uint32_t gateway;       /* Default gateway (host byte order) */
    uint32_t dns;           /* Primary DNS server (host byte order) */
    uint32_t server_ip;     /* DHCP server IP (host byte order) */
    uint32_t lease_time;    /* Lease duration in seconds */
    uint64_t lease_start;   /* hal_timer_ms() at time of ACK */
} dhcp_config_t;

/* Run the full DHCP sequence: DISCOVER -> OFFER -> REQUEST -> ACK.
 * Retries up to 3 times with exponential backoff (1s, 2s, 4s).
 * On success, fills *cfg and configures the TCP stack.
 * Returns HAL_OK or HAL_TIMEOUT / HAL_NO_DEVICE. */
hal_status_t dhcp_init(dhcp_config_t *cfg);

/* Send a DHCP DISCOVER and wait for an OFFER.
 * Lower-level; prefer dhcp_init() unless you need manual control.
 * On success, fills cfg->ip with the offered address. */
hal_status_t dhcp_discover_offer(dhcp_config_t *cfg);

/* Send a DHCP REQUEST for the offered IP and wait for ACK.
 * Must be called after a successful dhcp_discover_offer(). */
hal_status_t dhcp_request_ack(dhcp_config_t *cfg);

/* Check whether the current lease has expired.
 * Returns 1 if expired or no lease held, 0 if valid. */
int dhcp_lease_expired(const dhcp_config_t *cfg);

/* Get the cached DHCP config from the last successful init.
 * Returns NULL if no lease has been obtained. */
const dhcp_config_t *dhcp_get_config(void);

#endif /* ALJEFRA_KERNEL_DHCP_H */
