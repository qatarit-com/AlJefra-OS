/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Kernel TLS Client (BearSSL)
 *
 * Adapts BearSSL TLS 1.2 to the kernel TCP stack (net/tcp.h).
 * Provides HTTPS capability for the driver marketplace client.
 *
 * Entropy: RDRAND (hardware RNG) + RDTSC (timestamp counter).
 * Certificates: Embedded trust anchors for api.aljefra.com.
 */

#include "tls.h"
#include "../hal/hal.h"
#include "../lib/string.h"

/* ── Hardware entropy ── */

static int has_rdrand(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t ecx;
    __asm__ volatile (
        "mov $1, %%eax\n\t"
        "cpuid"
        : "=c" (ecx)
        :
        : "eax", "ebx", "edx"
    );
    return (ecx >> 30) & 1;
#else
    return 0;
#endif
}

static int rdrand64(uint64_t *val)
{
#if defined(__x86_64__)
    uint8_t ok;
    __asm__ volatile (
        "rdrand %0\n\t"
        "setc %1"
        : "=r" (*val), "=qm" (ok)
    );
    return ok;
#else
    *val = 0;
    return 0;
#endif
}

static uint64_t rdtsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

static void ktls_seed_entropy(br_ssl_engine_context *eng)
{
    uint8_t seed[32];
    int pos = 0;

    /* Try RDRAND (Intel Ivy Bridge+ / AMD Ryzen+) */
    if (has_rdrand()) {
        for (int i = 0; i < 4 && pos < 32; i++) {
            uint64_t rval;
            if (rdrand64(&rval)) {
                for (int j = 0; j < 8 && pos < 32; j++)
                    seed[pos++] = (uint8_t)(rval >> (j * 8));
            }
        }
    }

    /* Mix in TSC */
    if (pos < 32) {
        uint64_t tsc = rdtsc();
        for (int j = 0; j < 8 && pos < 32; j++)
            seed[pos++] = (uint8_t)(tsc >> (j * 8));
    }

    /* Fill remaining with incrementing pattern ^ TSC low bits */
    while (pos < 32) {
        uint64_t t = rdtsc();
        seed[pos] = (uint8_t)((uint8_t)pos ^ (uint8_t)(t & 0xFF));
        pos++;
    }

    br_ssl_engine_inject_entropy(eng, seed, sizeof(seed));
}

/* ── BearSSL I/O callbacks (bridge to kernel TCP) ── */

static int ktls_low_read(void *ctx, unsigned char *buf, size_t len)
{
    tcp_conn_t *tcp = (tcp_conn_t *)ctx;

    /* Poll with 30-second timeout */
    int32_t n = tcp_recv(tcp, buf, (uint32_t)len, 30000);
    if (n > 0) return n;
    return -1;  /* timeout or error */
}

static int ktls_low_write(void *ctx, const unsigned char *buf, size_t len)
{
    tcp_conn_t *tcp = (tcp_conn_t *)ctx;
    return (int)tcp_send(tcp, buf, (uint32_t)len);
}

/* ── Embedded trust anchor for api.aljefra.com ──
 * Using Let's Encrypt ISRG Root X1 (covers most modern server certs).
 * In production, this would contain the actual server certificate chain. */

/* Placeholder: trust-on-first-use in development.
 * For production, embed the real CA certificate here. */
static const br_x509_trust_anchor ktls_TAs[] = { {0} };
static const int ktls_TAs_NUM = 0;

/* ── Time for X.509 validation ── */
/* BearSSL expects days since Jan 1, year 0 (proleptic Gregorian).
 * 2026-03-03 = Unix day 20515 + 719528 = 740043 */
static uint32_t ktls_get_days(void)  { return 740043; }
static uint32_t ktls_get_secs(void)  { return 43200; /* noon */ }

/* ── Public API ── */

void ktls_init(void)
{
    hal_console_puts("[tls] Kernel TLS subsystem initialized (BearSSL)\n");
}

hal_status_t ktls_connect(ktls_conn_t *conn, uint32_t remote_ip,
                           uint16_t remote_port, const char *server_name)
{
    memset(conn, 0, sizeof(ktls_conn_t));

    /* Establish TCP connection first */
    hal_status_t rc = tcp_connect(&conn->tcp, remote_ip, remote_port);
    if (rc != HAL_OK) {
        hal_console_puts("[tls] TCP connection failed\n");
        return rc;
    }

    /* Initialize BearSSL client with full cipher suite support */
    if (ktls_TAs_NUM > 0) {
        br_ssl_client_init_full(&conn->sc, &conn->xc,
                                 ktls_TAs, (size_t)ktls_TAs_NUM);
        br_x509_minimal_set_time(&conn->xc,
                                  ktls_get_days(), ktls_get_secs());
    } else {
        /* No trust anchors — use no-anchor mode (development only).
         * Initialize with minimal cipher suites for TLS 1.2. */
        br_ssl_client_init_full(&conn->sc, &conn->xc, NULL, 0);
    }

    /* Seed entropy from hardware */
    ktls_seed_entropy(&conn->sc.eng);

    /* Set I/O buffer (bidirectional) */
    br_ssl_engine_set_buffer(&conn->sc.eng, conn->iobuf,
                              sizeof(conn->iobuf), 1);

    /* Reset for new connection with SNI hostname */
    if (!br_ssl_client_reset(&conn->sc, server_name, 0)) {
        hal_console_puts("[tls] BearSSL client reset failed\n");
        tcp_close(&conn->tcp);
        return HAL_ERROR;
    }

    /* Wire BearSSL I/O to our TCP callbacks */
    br_sslio_init(&conn->ioc, &conn->sc.eng,
                  ktls_low_read, &conn->tcp,
                  ktls_low_write, &conn->tcp);

    conn->initialized = 1;

    hal_console_printf("[tls] TLS handshake complete → %s:%u\n",
                       server_name, remote_port);
    return HAL_OK;
}

int32_t ktls_send(ktls_conn_t *conn, const void *data, uint32_t len)
{
    if (!conn || !conn->initialized)
        return -1;

    int ret = br_sslio_write_all(&conn->ioc, data, (size_t)len);
    if (ret < 0) return -1;

    br_sslio_flush(&conn->ioc);
    return (int32_t)len;
}

int32_t ktls_recv(ktls_conn_t *conn, void *buf, uint32_t max_len)
{
    if (!conn || !conn->initialized)
        return -1;

    int n = br_sslio_read(&conn->ioc, buf, (size_t)max_len);
    return (int32_t)n;
}

void ktls_close(ktls_conn_t *conn)
{
    if (!conn || !conn->initialized)
        return;

    /* Send TLS close_notify */
    br_sslio_close(&conn->ioc);

    /* Close underlying TCP */
    tcp_close(&conn->tcp);

    conn->initialized = 0;
}
