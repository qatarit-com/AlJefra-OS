/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Kernel TLS Client (BearSSL)
 *
 * Wraps BearSSL TLS 1.2 around the kernel TCP stack (net/tcp.h).
 * Used by the marketplace client for production HTTPS connections.
 */

#ifndef ALJEFRA_KERNEL_TLS_H
#define ALJEFRA_KERNEL_TLS_H

#include <stdint.h>
#include "tcp.h"
#include "../programs/netstack/bearssl/inc/bearssl.h"

/* Kernel TLS connection state */
typedef struct {
    tcp_conn_t       tcp;             /* Underlying kernel TCP connection */
    br_ssl_client_context sc;         /* BearSSL client context */
    br_x509_minimal_context xc;       /* X.509 certificate validator */
    br_sslio_context ioc;             /* BearSSL I/O context */
    uint8_t          iobuf[BR_SSL_BUFSIZE_BIDI]; /* ~33 KB I/O buffer */
    int              initialized;
} ktls_conn_t;

/* Initialize kernel TLS subsystem (call once at boot) */
void ktls_init(void);

/* Open a TLS connection to remote_ip:remote_port.
 * server_name is the SNI hostname (e.g. "api.aljefra.com").
 * Returns HAL_OK on success. */
hal_status_t ktls_connect(ktls_conn_t *conn, uint32_t remote_ip,
                           uint16_t remote_port, const char *server_name);

/* Send data over TLS. Returns bytes sent or -1 on error. */
int32_t ktls_send(ktls_conn_t *conn, const void *data, uint32_t len);

/* Receive data over TLS. Returns bytes received or -1 on error. */
int32_t ktls_recv(ktls_conn_t *conn, void *buf, uint32_t max_len);

/* Close TLS connection (sends close_notify + TCP close). */
void ktls_close(ktls_conn_t *conn);

#endif /* ALJEFRA_KERNEL_TLS_H */
