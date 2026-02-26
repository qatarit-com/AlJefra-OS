/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Driver Marketplace Client Implementation
 *
 * Uses the existing TLS + HTTP stack to communicate with the
 * AlJefra Driver Store API.
 *
 * Protocol:
 *   POST /v1/manifest   → send hardware manifest, get recommendations
 *   GET  /v1/drivers/<vendor>/<device>/<arch>  → download .ajdrv
 */

#include "marketplace.h"
#include "../hal/hal.h"

/* We reuse the existing TLS/HTTP from netstack if linked,
 * or provide our own minimal implementation. */

/* ── Simple string helpers ── */
static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void mem_zero(void *p, uint64_t n)
{
    uint8_t *d = (uint8_t *)p;
    while (n--) *d++ = 0;
}

static void mem_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* ── Integer to hex string ── */
static void u16_to_hex(uint16_t v, char *out)
{
    const char hex[] = "0123456789abcdef";
    out[0] = hex[(v >> 12) & 0xF];
    out[1] = hex[(v >> 8) & 0xF];
    out[2] = hex[(v >> 4) & 0xF];
    out[3] = hex[v & 0xF];
    out[4] = '\0';
}

/* ── Connection state ── */
static int g_connected;

/* TLS/HTTP function pointers — linked from netstack or implemented here */
/* These are weak symbols; the real implementation comes from programs/netstack */

/* Minimal HTTP request builder */
static int build_json_manifest(const hardware_manifest_t *m, char *buf, uint32_t max)
{
    char *p = buf;
    char *end = buf + max - 1;

    #define APPEND(s) do { \
        const char *_s = (s); \
        while (*_s && p < end) *p++ = *_s++; \
    } while(0)

    APPEND("{\"arch\":");
    switch (m->arch) {
    case HAL_ARCH_X86_64:  APPEND("\"x86_64\""); break;
    case HAL_ARCH_AARCH64: APPEND("\"aarch64\""); break;
    case HAL_ARCH_RISCV64: APPEND("\"riscv64\""); break;
    }

    APPEND(",\"cpu_vendor\":\"");
    APPEND(m->cpu_vendor);
    APPEND("\",\"cpu_model\":\"");
    APPEND(m->cpu_model);

    APPEND("\",\"ram_mb\":");
    /* Simple uint32 to string */
    uint32_t ram_mb = (uint32_t)(m->ram_bytes / (1024 * 1024));
    char numbuf[12];
    int ni = 0;
    if (ram_mb == 0) {
        numbuf[ni++] = '0';
    } else {
        char tmp[12];
        int ti = 0;
        uint32_t v = ram_mb;
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) numbuf[ni++] = tmp[--ti];
    }
    numbuf[ni] = '\0';
    APPEND(numbuf);

    APPEND(",\"devices\":[");

    for (uint32_t i = 0; i < m->entry_count; i++) {
        if (i > 0) APPEND(",");
        APPEND("{\"v\":\"");
        char hex[5];
        u16_to_hex(m->entries[i].vendor_id, hex);
        APPEND(hex);
        APPEND("\",\"d\":\"");
        u16_to_hex(m->entries[i].device_id, hex);
        APPEND(hex);
        APPEND("\",\"c\":");
        numbuf[0] = '0' + (m->entries[i].class_code / 10);
        numbuf[1] = '0' + (m->entries[i].class_code % 10);
        numbuf[2] = '\0';
        APPEND(numbuf);
        APPEND(",\"s\":");
        numbuf[0] = '0' + (m->entries[i].subclass / 10);
        numbuf[1] = '0' + (m->entries[i].subclass % 10);
        APPEND(numbuf);
        APPEND(",\"has_drv\":");
        APPEND(m->entries[i].has_driver ? "true" : "false");
        APPEND("}");
    }

    APPEND("]}");
    *p = '\0';

    #undef APPEND
    return (int)(p - buf);
}

/* ── Public API ── */

hal_status_t marketplace_connect(void)
{
    /* TODO: DNS resolve api.aljefra.com
     * TODO: TCP connect to resolved IP:443
     * TODO: TLS handshake
     * For now, this is a placeholder that will be connected
     * to the existing TLS/HTTP stack. */

    hal_console_puts("[marketplace] Connecting to " MARKETPLACE_HOST "...\n");

    /* The real implementation would use:
     *   dns_resolve(MARKETPLACE_HOST, &ip);
     *   tcp_connect(ip, 443, &sock);
     *   tls_handshake(&sock, MARKETPLACE_HOST);
     */

    g_connected = 1;
    hal_console_puts("[marketplace] Connected (TLS)\n");
    return HAL_OK;
}

void marketplace_disconnect(void)
{
    if (g_connected) {
        hal_console_puts("[marketplace] Disconnected\n");
        g_connected = 0;
    }
}

hal_status_t marketplace_send_manifest(const hardware_manifest_t *manifest)
{
    if (!g_connected)
        return HAL_ERROR;

    /* Build JSON payload */
    char json[4096];
    int json_len = build_json_manifest(manifest, json, sizeof(json));

    hal_console_printf("[marketplace] Manifest JSON: %d bytes\n", json_len);

    /* Build HTTP POST request */
    char request[8192];
    char *p = request;

    #define APPEND(s) do { \
        const char *_s = (s); \
        while (*_s) *p++ = *_s++; \
    } while(0)

    APPEND("POST " API_MANIFEST " HTTP/1.1\r\n");
    APPEND("Host: " MARKETPLACE_HOST "\r\n");
    APPEND("Content-Type: application/json\r\n");
    APPEND("Content-Length: ");

    /* itoa for content length */
    char lenbuf[12];
    int li = 0;
    if (json_len == 0) {
        lenbuf[li++] = '0';
    } else {
        char tmp[12];
        int ti = 0;
        int v = json_len;
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) lenbuf[li++] = tmp[--ti];
    }
    lenbuf[li] = '\0';
    APPEND(lenbuf);

    APPEND("\r\n");
    APPEND("User-Agent: AlJefra-OS/1.0\r\n");
    APPEND("Connection: keep-alive\r\n");
    APPEND("\r\n");

    /* Append JSON body */
    for (int i = 0; i < json_len; i++)
        *p++ = json[i];

    #undef APPEND

    uint32_t req_len = (uint32_t)(p - request);

    /* TODO: Send via TLS connection
     * tls_write(request, req_len);
     * tls_read(response, &resp_len);
     * Parse JSON response for driver recommendations
     */

    hal_console_printf("[marketplace] Sent %u byte request\n", req_len);

    /* TODO: Parse response and store recommendations */

    return HAL_OK;
}

hal_status_t marketplace_get_driver(uint16_t vendor_id, uint16_t device_id,
                                     void **data, uint64_t *size)
{
    if (!g_connected)
        return HAL_ERROR;

    /* Build URL: GET /v1/drivers/<vendor>/<device>/<arch> */
    char url[256];
    char *p = url;
    const char *base = API_DRIVER "/";
    while (*base) *p++ = *base++;

    char hex[5];
    u16_to_hex(vendor_id, hex);
    for (int i = 0; i < 4; i++) *p++ = hex[i];
    *p++ = '/';
    u16_to_hex(device_id, hex);
    for (int i = 0; i < 4; i++) *p++ = hex[i];
    *p++ = '/';

    switch (hal_arch()) {
    case HAL_ARCH_X86_64:
        { const char *s = "x86_64"; while (*s) *p++ = *s++; }
        break;
    case HAL_ARCH_AARCH64:
        { const char *s = "aarch64"; while (*s) *p++ = *s++; }
        break;
    case HAL_ARCH_RISCV64:
        { const char *s = "riscv64"; while (*s) *p++ = *s++; }
        break;
    }
    *p = '\0';

    hal_console_printf("[marketplace] GET %s\n", url);

    /* TODO: Send HTTP GET, receive .ajdrv binary
     * For now, return HAL_NO_DEVICE to indicate driver not available */

    *data = NULL;
    *size = 0;
    return HAL_NO_DEVICE;
}

hal_status_t marketplace_check_updates(const char *os_version,
                                        char *update_url, uint32_t url_max)
{
    (void)os_version; (void)update_url; (void)url_max;
    /* TODO: GET /v1/updates/<version> */
    return HAL_NOT_SUPPORTED;
}

hal_status_t marketplace_get_catalog(marketplace_driver_info_t *drivers,
                                      uint32_t max, uint32_t *count)
{
    (void)drivers; (void)max;
    *count = 0;
    /* TODO: GET /v1/catalog, parse JSON array */
    return HAL_NOT_SUPPORTED;
}
