/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Minimal DNS Resolver */

#ifndef ALJEFRA_DNS_H
#define ALJEFRA_DNS_H

#include <stdint.h>
#include "../hal/hal.h"

/* Resolve an A-record. Returns HAL_OK on success. */
hal_status_t dns_resolve(const char *hostname, uint32_t dns_server, uint32_t *out_ip);

#endif /* ALJEFRA_DNS_H */
