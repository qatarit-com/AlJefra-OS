/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Over-The-Air (OTA) Update Implementation
 *
 * Full OTA pipeline for bare-metal kernel updates:
 *   check -> download -> verify -> apply
 *
 * All freestanding C.  No malloc, no stdlib.
 * Uses a static 256 KB staging buffer for the downloaded package.
 *
 * Network: uses TCP (net/tcp.h) for HTTP GET from marketplace.
 * Storage: uses the storage driver (driver_loader.h) for writing.
 * Crypto:  uses Ed25519 verification (store/verify.h).
 */

#include "ota.h"
#include "driver_loader.h"
#include "ed25519_key.h"
#include "../store/verify.h"
#include "../store/package.h"
#include "../net/tcp.h"
#include "../net/checksum.h"
#include "../lib/string.h"
#include "../lib/endian.h"

/* ── Constants ── */

/* Marketplace server for update checks */
#define OTA_MARKETPLACE_IP      0x0A000001  /* 10.0.0.1 (configurable) */
#define OTA_MARKETPLACE_PORT    8080
#define OTA_HTTP_TIMEOUT_MS     10000
#define OTA_CONNECT_TIMEOUT_MS  5000

/* Storage: the kernel image is written starting at LBA sector 10
 * (matches the two-stage boot architecture from MEMORY.md). */
#define OTA_KERNEL_START_LBA    10
#define OTA_SECTOR_SIZE         4096

/* Rollback flag is stored at a known LBA before the kernel. */
#define OTA_ROLLBACK_LBA        9
#define OTA_ROLLBACK_MAGIC      0x524F4C4C  /* "ROLL" */

/* ── Static staging buffer (256 KB, no heap) ── */
static uint8_t g_staging[OTA_STAGING_SIZE];
static uint32_t g_staging_len;

/* ── State ── */
static ota_status_t g_status = OTA_NONE;

/* ── CRC32 (for download integrity, separate from Ed25519 signature) ── */

static uint32_t crc32_byte(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 1)
            crc = (crc >> 1) ^ 0xEDB88320;
        else
            crc >>= 1;
    }
    return crc;
}

static uint32_t crc32_buf(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_byte(crc, p[i]);
    return crc ^ 0xFFFFFFFF;
}

/* ── Minimal HTTP GET over TCP ── */

/* Format a decimal number into a buffer, return bytes written */
static uint32_t fmt_u32(char *buf, uint32_t val)
{
    char tmp[12];
    int n = 0;
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    while (val > 0) {
        tmp[n++] = '0' + (char)(val % 10);
        val /= 10;
    }
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return (uint32_t)n;
}

/* Build an HTTP GET request into buf, return length */
static uint32_t build_http_get(char *buf, uint32_t buf_size,
                                const char *path, const char *host)
{
    uint32_t pos = 0;

    /* "GET <path> HTTP/1.1\r\n" */
    const char *prefix = "GET ";
    for (uint32_t i = 0; prefix[i] && pos < buf_size - 1; i++)
        buf[pos++] = prefix[i];
    for (uint32_t i = 0; path[i] && pos < buf_size - 1; i++)
        buf[pos++] = path[i];
    const char *ver = " HTTP/1.1\r\n";
    for (uint32_t i = 0; ver[i] && pos < buf_size - 1; i++)
        buf[pos++] = ver[i];

    /* "Host: <host>\r\n" */
    const char *hdr = "Host: ";
    for (uint32_t i = 0; hdr[i] && pos < buf_size - 1; i++)
        buf[pos++] = hdr[i];
    for (uint32_t i = 0; host[i] && pos < buf_size - 1; i++)
        buf[pos++] = host[i];
    buf[pos++] = '\r'; buf[pos++] = '\n';

    /* "Connection: close\r\n\r\n" */
    const char *close = "Connection: close\r\n\r\n";
    for (uint32_t i = 0; close[i] && pos < buf_size - 1; i++)
        buf[pos++] = close[i];

    buf[pos] = '\0';
    return pos;
}

/* Parse Content-Length from HTTP response headers.
 * Returns content length, or 0 if not found. */
static uint32_t parse_content_length(const uint8_t *buf, uint32_t len)
{
    const char *needle = "Content-Length: ";
    uint32_t nlen = 16; /* length of "Content-Length: " */

    for (uint32_t i = 0; i + nlen < len; i++) {
        bool match = true;
        for (uint32_t j = 0; j < nlen; j++) {
            char c = (char)buf[i + j];
            /* Case-insensitive compare */
            if (c >= 'A' && c <= 'Z') c += 32;
            char n = needle[j];
            if (n >= 'A' && n <= 'Z') n += 32;
            if (c != n) { match = false; break; }
        }
        if (match) {
            uint32_t val = 0;
            for (uint32_t k = i + nlen; k < len; k++) {
                char c = (char)buf[k];
                if (c >= '0' && c <= '9')
                    val = val * 10 + (uint32_t)(c - '0');
                else
                    break;
            }
            return val;
        }
    }
    return 0;
}

/* Find the end of HTTP headers (\r\n\r\n), return offset past them.
 * Returns 0 if not found. */
static uint32_t find_header_end(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    }
    return 0;
}

/* Parse HTTP status code from first line (e.g. "HTTP/1.1 200 OK") */
static int parse_http_status(const uint8_t *buf, uint32_t len)
{
    /* Find the first space */
    uint32_t i = 0;
    while (i < len && buf[i] != ' ') i++;
    i++; /* Skip space */

    int code = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        code = code * 10 + (buf[i] - '0');
        i++;
    }
    return code;
}

/* ── Simple JSON field extractor ──
 *    Finds "key":"value" in a JSON blob, copies value to out.
 *    No nesting support.  Sufficient for {"version":"1.0.1","url":"..."} */

static bool json_get_string(const char *json, uint32_t json_len,
                             const char *key, char *out, uint32_t out_max)
{
    uint32_t klen = str_len(key);
    out[0] = '\0';

    for (uint32_t i = 0; i + klen + 4 < json_len; i++) {
        if (json[i] != '"') continue;

        /* Check if key matches */
        bool match = true;
        for (uint32_t j = 0; j < klen; j++) {
            if (json[i + 1 + j] != key[j]) { match = false; break; }
        }
        if (!match || json[i + 1 + klen] != '"')
            continue;

        /* Found key, skip to value */
        uint32_t vi = i + 1 + klen + 1; /* past closing quote */
        while (vi < json_len && (json[vi] == ':' || json[vi] == ' '))
            vi++;

        if (vi < json_len && json[vi] == '"') {
            vi++; /* Opening quote of value */
            uint32_t o = 0;
            while (vi < json_len && json[vi] != '"' && o < out_max - 1) {
                out[o++] = json[vi++];
            }
            out[o] = '\0';
            return true;
        }

        /* Numeric value */
        {
            uint32_t o = 0;
            while (vi < json_len && json[vi] != ',' && json[vi] != '}' &&
                   json[vi] != ' ' && o < out_max - 1) {
                out[o++] = json[vi++];
            }
            out[o] = '\0';
            return true;
        }
    }

    return false;
}

/* Parse a decimal string to uint32 */
static uint32_t str_to_u32(const char *s)
{
    uint32_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return val;
}

/* Parse a hex string to uint32 */
static uint32_t str_to_hex32(const char *s)
{
    uint32_t val = 0;
    /* Skip optional "0x" prefix */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        char c = *s;
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')  d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  d = (uint32_t)(c - 'A' + 10);
        else break;
        val = (val << 4) | d;
        s++;
    }
    return val;
}

/* ── Public API ── */

hal_status_t ota_check(const char *current_version, ota_update_info_t *info)
{
    if (!info)
        return HAL_ERROR;

    memset(info, 0, sizeof(*info));
    g_status = OTA_NONE;

    hal_console_puts("[ota] Checking for updates...\n");

    /* Build GET request: /v1/updates/latest?version=<current> */
    char path[128];
    uint32_t pi = 0;
    const char *base = "/v1/updates/latest?version=";
    for (uint32_t i = 0; base[i]; i++)
        path[pi++] = base[i];
    for (uint32_t i = 0; current_version[i] && pi < sizeof(path) - 1; i++)
        path[pi++] = current_version[i];
    path[pi] = '\0';

    /* Connect to marketplace */
    tcp_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    hal_status_t rc = tcp_connect(&conn, OTA_MARKETPLACE_IP, OTA_MARKETPLACE_PORT);
    if (rc != HAL_OK) {
        hal_console_puts("[ota] Cannot connect to marketplace\n");
        g_status = OTA_ERROR;
        return HAL_TIMEOUT;
    }

    /* Send HTTP GET */
    char req[512];
    uint32_t req_len = build_http_get(req, sizeof(req), path, "store.aljefra.com");
    tcp_send(&conn, req, req_len);

    /* Receive response */
    uint8_t resp[2048];
    int32_t rlen = tcp_recv(&conn, resp, sizeof(resp), OTA_HTTP_TIMEOUT_MS);
    tcp_close(&conn);

    if (rlen <= 0) {
        hal_console_puts("[ota] No response from marketplace\n");
        g_status = OTA_ERROR;
        return HAL_TIMEOUT;
    }

    /* Parse HTTP status */
    int status = parse_http_status(resp, (uint32_t)rlen);
    if (status == 204 || status == 404) {
        hal_console_puts("[ota] No update available\n");
        return HAL_ERROR; /* No update */
    }
    if (status != 200) {
        hal_console_printf("[ota] Unexpected HTTP status %d\n", status);
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    /* Find body */
    uint32_t body_off = find_header_end(resp, (uint32_t)rlen);
    if (body_off == 0) {
        hal_console_puts("[ota] Malformed HTTP response\n");
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    /* Parse JSON body: {"version":"1.0.1","url":"/v1/updates/1.0.1","size":12345,"crc32":"AABBCCDD"} */
    const char *json = (const char *)(resp + body_off);
    uint32_t json_len = (uint32_t)rlen - body_off;

    char tmp[256];

    if (!json_get_string(json, json_len, "version", info->version, sizeof(info->version))) {
        hal_console_puts("[ota] Missing 'version' in response\n");
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    if (json_get_string(json, json_len, "url", info->url, sizeof(info->url))) {
        /* Got URL */
    }

    if (json_get_string(json, json_len, "size", tmp, sizeof(tmp))) {
        info->size = str_to_u32(tmp);
    }

    if (json_get_string(json, json_len, "crc32", tmp, sizeof(tmp))) {
        info->crc32 = str_to_hex32(tmp);
    }

    hal_console_printf("[ota] Update available: v%s (%u bytes)\n",
                       info->version, info->size);

    g_status = OTA_AVAILABLE;
    return HAL_OK;
}

hal_status_t ota_download(const ota_update_info_t *info)
{
    if (!info || info->url[0] == '\0')
        return HAL_ERROR;

    if (info->size > OTA_STAGING_SIZE) {
        hal_console_printf("[ota] Package too large: %u > %u\n",
                           info->size, OTA_STAGING_SIZE);
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    g_status = OTA_DOWNLOADING;
    hal_console_printf("[ota] Downloading from %s...\n", info->url);

    /* Connect to marketplace */
    tcp_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    hal_status_t rc = tcp_connect(&conn, OTA_MARKETPLACE_IP, OTA_MARKETPLACE_PORT);
    if (rc != HAL_OK) {
        hal_console_puts("[ota] Download connect failed\n");
        g_status = OTA_ERROR;
        return HAL_TIMEOUT;
    }

    /* Send HTTP GET for the package binary */
    char req[512];
    uint32_t req_len = build_http_get(req, sizeof(req), info->url, "store.aljefra.com");
    tcp_send(&conn, req, req_len);

    /* Receive response headers first */
    uint8_t hdr_buf[1024];
    int32_t hlen = tcp_recv(&conn, hdr_buf, sizeof(hdr_buf), OTA_HTTP_TIMEOUT_MS);
    if (hlen <= 0) {
        tcp_close(&conn);
        hal_console_puts("[ota] No response during download\n");
        g_status = OTA_ERROR;
        return HAL_TIMEOUT;
    }

    int status = parse_http_status(hdr_buf, (uint32_t)hlen);
    if (status != 200) {
        tcp_close(&conn);
        hal_console_printf("[ota] Download HTTP status %d\n", status);
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    uint32_t content_len = parse_content_length(hdr_buf, (uint32_t)hlen);
    uint32_t body_off = find_header_end(hdr_buf, (uint32_t)hlen);

    if (content_len == 0)
        content_len = info->size; /* Fallback to expected size */

    if (content_len > OTA_STAGING_SIZE) {
        tcp_close(&conn);
        hal_console_puts("[ota] Content too large for staging buffer\n");
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    /* Copy any body data already received in the header buffer */
    g_staging_len = 0;
    if (body_off > 0 && body_off < (uint32_t)hlen) {
        uint32_t initial = (uint32_t)hlen - body_off;
        if (initial > OTA_STAGING_SIZE)
            initial = OTA_STAGING_SIZE;
        memcpy(g_staging, hdr_buf + body_off, initial);
        g_staging_len = initial;
    }

    /* Receive remaining body data */
    uint32_t progress_pct = 0;
    while (g_staging_len < content_len) {
        uint32_t remaining = content_len - g_staging_len;
        uint32_t chunk = remaining;
        if (chunk > 4096) chunk = 4096;

        int32_t n = tcp_recv(&conn, g_staging + g_staging_len, chunk,
                             OTA_HTTP_TIMEOUT_MS);
        if (n <= 0) {
            /* Timeout or connection closed */
            if (g_staging_len >= content_len)
                break; /* Might have all data */
            tcp_close(&conn);
            hal_console_printf("[ota] Download stalled at %u/%u bytes\n",
                               g_staging_len, content_len);
            g_status = OTA_ERROR;
            return HAL_TIMEOUT;
        }

        g_staging_len += (uint32_t)n;

        /* Progress reporting every 10% */
        uint32_t pct = (g_staging_len * 100) / content_len;
        if (pct / 10 > progress_pct / 10) {
            progress_pct = pct;
            hal_console_printf("[ota] Download: %u%%  (%u/%u bytes)\n",
                               pct, g_staging_len, content_len);
        }
    }

    tcp_close(&conn);

    /* Verify CRC32 if provided */
    if (info->crc32 != 0) {
        uint32_t actual_crc = crc32_buf(g_staging, g_staging_len);
        if (actual_crc != info->crc32) {
            hal_console_printf("[ota] CRC32 mismatch: expected 0x%08X, got 0x%08X\n",
                               info->crc32, actual_crc);
            g_status = OTA_ERROR;
            return HAL_ERROR;
        }
        hal_console_puts("[ota] CRC32 verified OK\n");
    }

    hal_console_printf("[ota] Download complete: %u bytes\n", g_staging_len);
    return HAL_OK;
}

hal_status_t ota_verify(void)
{
    if (g_staging_len == 0) {
        hal_console_puts("[ota] No package in staging buffer\n");
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    g_status = OTA_VERIFYING;
    hal_console_puts("[ota] Verifying Ed25519 signature...\n");

#if ALJEFRA_DEV_MODE
    hal_console_puts("[ota] WARNING: Dev mode — skipping signature verification\n");
    g_status = OTA_READY;
    return HAL_OK;
#else
    /* Ensure the trusted key is installed */
    ed25519_key_install();

    /* Verify the package signature using the store verification module.
     * ajdrv_verify() uses the trusted key set by ajdrv_set_trusted_key(). */
    hal_status_t rc = ajdrv_verify(g_staging, g_staging_len);
    if (rc != HAL_OK) {
        hal_console_puts("[ota] SIGNATURE VERIFICATION FAILED\n");
        hal_console_puts("[ota] Update rejected — possible tampering\n");
        g_status = OTA_ERROR;
        return HAL_ERROR;
    }

    hal_console_puts("[ota] Signature verified OK\n");
    g_status = OTA_READY;
    return HAL_OK;
#endif
}

hal_status_t ota_apply(void)
{
    if (g_status != OTA_READY) {
        hal_console_puts("[ota] Package not verified, cannot apply\n");
        return HAL_ERROR;
    }

    g_status = OTA_APPLYING;
    hal_console_puts("[ota] Applying update...\n");

    /* Get storage driver */
    const driver_ops_t *stor = driver_get_storage();
    if (!stor || !stor->write) {
        hal_console_puts("[ota] No storage driver available\n");
        g_status = OTA_ERROR;
        return HAL_NO_DEVICE;
    }

    /* Step 1: Set the rollback flag BEFORE writing the new kernel.
     * If the system crashes mid-write, the bootloader will see this
     * flag and know the kernel may be corrupt. */
    {
        uint8_t rollback_sector[OTA_SECTOR_SIZE];
        memset(rollback_sector, 0, OTA_SECTOR_SIZE);

        /* Write rollback magic at the start of the sector */
        rollback_sector[0] = (OTA_ROLLBACK_MAGIC >>  0) & 0xFF;
        rollback_sector[1] = (OTA_ROLLBACK_MAGIC >>  8) & 0xFF;
        rollback_sector[2] = (OTA_ROLLBACK_MAGIC >> 16) & 0xFF;
        rollback_sector[3] = (OTA_ROLLBACK_MAGIC >> 24) & 0xFF;

        /* Store the staging length so bootloader knows how big the image is */
        rollback_sector[4] = (g_staging_len >>  0) & 0xFF;
        rollback_sector[5] = (g_staging_len >>  8) & 0xFF;
        rollback_sector[6] = (g_staging_len >> 16) & 0xFF;
        rollback_sector[7] = (g_staging_len >> 24) & 0xFF;

        int64_t wr = stor->write(rollback_sector, OTA_ROLLBACK_LBA, 1);
        if (wr < 0) {
            hal_console_puts("[ota] Failed to write rollback flag\n");
            g_status = OTA_ERROR;
            return HAL_ERROR;
        }
    }

    /* Step 2: Write the kernel image sector by sector */
    uint32_t sectors = (g_staging_len + OTA_SECTOR_SIZE - 1) / OTA_SECTOR_SIZE;
    uint32_t lba = OTA_KERNEL_START_LBA;

    for (uint32_t i = 0; i < sectors; i++) {
        /* Prepare a full sector (pad with zeros if last sector is partial) */
        uint8_t sector[OTA_SECTOR_SIZE];
        uint32_t offset = i * OTA_SECTOR_SIZE;
        uint32_t chunk = g_staging_len - offset;
        if (chunk > OTA_SECTOR_SIZE) chunk = OTA_SECTOR_SIZE;

        memcpy(sector, g_staging + offset, chunk);
        if (chunk < OTA_SECTOR_SIZE)
            memset(sector + chunk, 0, OTA_SECTOR_SIZE - chunk);

        int64_t wr = stor->write(sector, lba + i, 1);
        if (wr < 0) {
            hal_console_printf("[ota] Write failed at LBA %u\n",
                               (uint32_t)(lba + i));
            g_status = OTA_ERROR;
            return HAL_ERROR;
        }
    }

    /* Step 3: Clear the rollback flag to indicate successful write */
    {
        uint8_t clear_sector[OTA_SECTOR_SIZE];
        memset(clear_sector, 0, OTA_SECTOR_SIZE);
        stor->write(clear_sector, OTA_ROLLBACK_LBA, 1);
    }

    hal_console_printf("[ota] Update applied: %u bytes across %u sectors\n",
                       g_staging_len, sectors);

    g_status = OTA_DONE;
    hal_console_puts("[ota] Update complete. Reboot to activate.\n");
    return HAL_OK;
}

hal_status_t ota_run(const char *current_version)
{
    ota_update_info_t info;
    hal_status_t rc;

    hal_console_puts("[ota] === OTA Update Pipeline ===\n");

    /* Step 1: Check */
    rc = ota_check(current_version, &info);
    if (rc != HAL_OK) {
        if (g_status != OTA_ERROR)
            hal_console_puts("[ota] No updates available\n");
        return rc;
    }

    /* Step 2: Download */
    rc = ota_download(&info);
    if (rc != HAL_OK) {
        hal_console_puts("[ota] Download failed\n");
        return rc;
    }

    /* Step 3: Verify */
    rc = ota_verify();
    if (rc != HAL_OK) {
        hal_console_puts("[ota] Verification failed\n");
        return rc;
    }

    /* Step 4: Apply */
    rc = ota_apply();
    if (rc != HAL_OK) {
        hal_console_puts("[ota] Apply failed\n");
        return rc;
    }

    hal_console_puts("[ota] === OTA Pipeline Complete ===\n");
    return HAL_OK;
}

ota_status_t ota_get_status(void)
{
    return g_status;
}

const char *ota_status_string(ota_status_t status)
{
    switch (status) {
    case OTA_NONE:        return "none";
    case OTA_AVAILABLE:   return "available";
    case OTA_DOWNLOADING: return "downloading";
    case OTA_VERIFYING:   return "verifying";
    case OTA_READY:       return "ready";
    case OTA_APPLYING:    return "applying";
    case OTA_DONE:        return "done";
    case OTA_ERROR:       return "error";
    default:              return "unknown";
    }
}
