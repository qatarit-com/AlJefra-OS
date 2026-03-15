/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Kernel-Level DHCP Client Implementation
 *
 * Provides the high-level DHCP API consumed by ai_bootstrap.c and ota.c.
 * Delegates raw packet construction and parsing to net/dhcp.c, adding:
 *   - Retry with exponential backoff
 *   - Lease caching and expiry tracking
 *   - TCP stack configuration after successful lease
 *
 * All bare-metal, freestanding.  No malloc, no stdlib.
 */

#include "dhcp.h"
#include "driver_loader.h"
#include "../net/dhcp.h"
#include "../net/tcp.h"
#include "../net/checksum.h"
#include "../lib/string.h"
#include "../lib/endian.h"

/* ── Constants ── */
#define DHCP_MAX_RETRIES    3
#define DHCP_BASE_TIMEOUT   1000   /* ms, doubles each retry */

/* ── Cached configuration ── */
static dhcp_config_t g_config;
static bool g_config_valid;
static char g_dhcp_status[160] = "DHCP has not run yet";

/* ── Helpers ── */

static void log_ip(const char *label, uint32_t ip)
{
    hal_console_printf("[dhcp] %s: %u.%u.%u.%u\n", label,
                       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                       (ip >> 8) & 0xFF, ip & 0xFF);
}

static void set_dhcp_status(const char *msg)
{
    str_copy(g_dhcp_status, msg ? msg : "DHCP status unavailable",
             sizeof(g_dhcp_status));
}

/* ── Low-level DHCP — delegates to net/dhcp.c ── */

/* The net/dhcp.c module provides:
 *   hal_status_t dhcp_discover(uint32_t *ip, uint32_t *gateway, uint32_t *dns);
 *   const dhcp_lease_t *dhcp_get_lease(void);
 *
 * dhcp_discover() runs the full DORA sequence internally and returns
 * ip/gateway/dns in host byte order.  dhcp_get_lease() gives the
 * full lease structure including subnet mask and lease time.
 */

/* ── Public API ── */

hal_status_t dhcp_discover_offer(dhcp_config_t *cfg)
{
    if (!cfg)
        return HAL_ERROR;

    /* Verify we have a network driver */
    const driver_ops_t *net = driver_get_network();
    if (!net || !net->net_tx || !net->net_rx)
        return HAL_NO_DEVICE;

    /* Delegate to net/dhcp.c — it does DISCOVER + waits for OFFER.
     * The net implementation actually does full DORA in one call,
     * but on timeout after DISCOVER we treat it as "no offer". */
    uint32_t ip = 0, gw = 0, dns = 0;
    hal_status_t rc = dhcp_discover(&ip, &gw, &dns);
    if (rc != HAL_OK)
        return rc;

    /* Populate config from net/dhcp lease */
    const dhcp_lease_t *lease = dhcp_get_lease();

    cfg->ip         = ip;
    cfg->gateway    = gw;
    cfg->dns        = dns;
    cfg->netmask    = lease ? lease->subnet_mask : 0xFFFFFF00; /* /24 default */
    cfg->server_ip  = lease ? lease->server_ip   : 0;
    cfg->lease_time = lease ? lease->lease_time   : 86400; /* 24h default */
    cfg->lease_start = hal_timer_ms();

    return HAL_OK;
}

hal_status_t dhcp_request_ack(dhcp_config_t *cfg)
{
    /* net/dhcp.c performs the full DORA in dhcp_discover(), so by the
     * time dhcp_discover_offer() returns HAL_OK, we already have the
     * ACK.  This function exists for API completeness. */
    if (!cfg || cfg->ip == 0)
        return HAL_ERROR;

    return HAL_OK;
}

hal_status_t dhcp_init(dhcp_config_t *cfg)
{
    if (!cfg)
        return HAL_ERROR;

    /* Check for network driver */
    const driver_ops_t *net = driver_get_network();
    if (!net) {
        hal_console_puts("[dhcp] No network driver available\n");
        set_dhcp_status("No network driver available for DHCP");
        return HAL_NO_DEVICE;
    }

    hal_console_puts("[dhcp] Starting DHCP client...\n");
    set_dhcp_status("Starting DHCP handshake");

    memset(cfg, 0, sizeof(*cfg));

    /* Retry with exponential backoff */
    uint32_t timeout = DHCP_BASE_TIMEOUT;
    hal_status_t rc = HAL_TIMEOUT;

    for (int attempt = 0; attempt < DHCP_MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            hal_console_printf("[dhcp] Retry %d/%d (waiting %u ms)...\n",
                               attempt + 1, DHCP_MAX_RETRIES, timeout);
            set_dhcp_status("Retrying DHCP after timeout");
            hal_timer_delay_ms(timeout);
            timeout *= 2; /* Exponential backoff */
        }

        rc = dhcp_discover_offer(cfg);
        if (rc == HAL_OK)
            break;

        if (rc == HAL_TIMEOUT)
            set_dhcp_status("DHCP timed out waiting for reply");
        else if (rc == HAL_NO_DEVICE)
            set_dhcp_status("Network driver is active but cannot send or receive");
        else
            set_dhcp_status("DHCP handshake failed before lease was assigned");
    }

    if (rc != HAL_OK) {
        hal_console_puts("[dhcp] All retries exhausted, DHCP failed\n");
        g_config_valid = false;
        if (rc == HAL_TIMEOUT)
            set_dhcp_status("DHCP failed after multiple retries");
        return rc;
    }

    /* Log the obtained configuration */
    hal_console_puts("[dhcp] Lease obtained:\n");
    log_ip("  IP Address ", cfg->ip);
    log_ip("  Subnet Mask", cfg->netmask);
    log_ip("  Gateway    ", cfg->gateway);
    log_ip("  DNS Server ", cfg->dns);
    hal_console_printf("[dhcp]   Lease Time : %u seconds\n", cfg->lease_time);

    /* Configure TCP stack with the DHCP-obtained parameters */
    tcp_init(cfg->ip, cfg->gateway, cfg->netmask);

    /* Cache the configuration */
    g_config = *cfg;
    g_config_valid = true;
    set_dhcp_status("DHCP lease acquired successfully");

    hal_console_puts("[dhcp] Network configured successfully\n");
    return HAL_OK;
}

int dhcp_lease_expired(const dhcp_config_t *cfg)
{
    if (!cfg || cfg->ip == 0 || cfg->lease_time == 0)
        return 1;

    uint64_t now = hal_timer_ms();
    uint64_t elapsed_s = (now - cfg->lease_start) / 1000;

    /* Lease expired if elapsed time exceeds lease duration */
    if (elapsed_s >= cfg->lease_time)
        return 1;

    return 0;
}

const dhcp_config_t *dhcp_get_config(void)
{
    if (!g_config_valid)
        return NULL;
    return &g_config;
}

const char *dhcp_last_status_message(void)
{
    return g_dhcp_status;
}
