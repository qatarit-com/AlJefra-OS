/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Minimal DNS Resolver */

#include "dns.h"
#include "checksum.h"
#include "tcp.h"
#include "../hal/hal.h"
#include "../kernel/driver_loader.h"
#include "../lib/string.h"
#include "../lib/endian.h"

extern uint32_t g_local_ip;
extern uint32_t g_gateway;
extern uint32_t g_netmask;
extern uint8_t  g_local_mac[6];

typedef struct __attribute__((packed)) {
    uint8_t  eth_dst[6];
    uint8_t  eth_src[6];
    uint16_t eth_type;
    uint8_t  ip_ver_ihl;
    uint8_t  ip_tos;
    uint16_t ip_len;
    uint16_t ip_id;
    uint16_t ip_frag;
    uint8_t  ip_ttl;
    uint8_t  ip_proto;
    uint16_t ip_checksum;
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t udp_src;
    uint16_t udp_dst;
    uint16_t udp_len;
    uint16_t udp_checksum;
} net_header_t;

static uint32_t skip_name(const uint8_t *pkt, uint32_t len, uint32_t off) {
    while (off < len) {
        uint8_t c = pkt[off];
        if (c == 0) return off + 1;
        if ((c & 0xC0) == 0xC0) return off + 2;
        off += c + 1;
    }
    return off;
}

hal_status_t dns_resolve(const char *hostname, uint32_t dns_server, uint32_t *out_ip)
{
    const driver_ops_t *net = driver_get_network();
    if (!net || !net->net_tx || !net->net_rx) return HAL_NO_DEVICE;

    uint32_t next_hop = ((dns_server & g_netmask) == (g_local_ip & g_netmask)) ? dns_server : g_gateway;
    uint8_t dest_mac[6];
    hal_status_t rc = arp_resolve(next_hop, dest_mac);
    if (rc != HAL_OK) return rc;

    uint8_t frame[512];
    memset(frame, 0, sizeof(frame));
    net_header_t *hdr = (net_header_t *)frame;
    
    memcpy(hdr->eth_dst, dest_mac, 6);
    memcpy(hdr->eth_src, g_local_mac, 6);
    hdr->eth_type = htons(0x0800);
    
    uint8_t *dns_payload = frame + sizeof(net_header_t);
    uint32_t txid = (uint32_t)hal_timer_ns() & 0xFFFF;
    dns_payload[0] = (txid >> 8) & 0xFF; /* ID */
    dns_payload[1] = txid & 0xFF;
    dns_payload[2] = 0x01; /* Flags: Standard Query */
    dns_payload[3] = 0x00;
    dns_payload[4] = 0x00; /* QDCOUNT = 1 */
    dns_payload[5] = 0x01;
    dns_payload[6] = 0x00; /* ANCOUNT */
    dns_payload[7] = 0x00;
    dns_payload[8] = 0x00; /* NSCOUNT */
    dns_payload[9] = 0x00;
    dns_payload[10] = 0x00; /* ARCOUNT */
    dns_payload[11] = 0x00;
    
    uint32_t off = 12;
    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        uint32_t len = dot - p;
        dns_payload[off++] = len;
        memcpy(&dns_payload[off], p, len);
        off += len;
        if (*dot) p = dot + 1;
        else break;
    }
    dns_payload[off++] = 0; /* Null root label */
    
    /* QTYPE = A (1) */
    dns_payload[off++] = 0x00;
    dns_payload[off++] = 0x01;
    /* QCLASS = IN (1) */
    dns_payload[off++] = 0x00;
    dns_payload[off++] = 0x01;
    
    uint16_t udp_total = 8 + off;
    uint16_t ip_total = 20 + udp_total;
    
    hdr->ip_ver_ihl = 0x45;
    hdr->ip_len = htons(ip_total);
    hdr->ip_id = htons(1);
    hdr->ip_ttl = 64;
    hdr->ip_proto = 17;
    hdr->ip_src = htonl(g_local_ip);
    hdr->ip_dst = htonl(dns_server);
    hdr->ip_checksum = ip_checksum(&hdr->ip_ver_ihl, 20);
    
    hdr->udp_src = htons(49152 + (txid & 0x3FFF));
    hdr->udp_dst = htons(53);
    hdr->udp_len = htons(udp_total);
    
    uint32_t total = sizeof(net_header_t) + off;
    if (total < 60) total = 60;
    
    hal_console_printf("[dns] Querying %s via %u.%u.%u.%u...\n", hostname,
                       (dns_server >> 24) & 0xFF, (dns_server >> 16) & 0xFF,
                       (dns_server >> 8) & 0xFF, dns_server & 0xFF);
    
    net->net_tx(frame, total);
    
    /* Wait for response */
    uint64_t start = hal_timer_ms();
    while (hal_timer_ms() - start < 5000) {
        uint8_t rx[1500];
        int64_t rlen = net->net_rx(rx, sizeof(rx));
        if (rlen <= 0) { hal_timer_delay_us(100); continue; }
        if (rlen < (int64_t)sizeof(net_header_t)) continue;
        net_header_t *rh = (net_header_t *)rx;
        if (ntohs(rh->eth_type) != 0x0800 || rh->ip_proto != 17) continue;
        if (ntohs(rh->udp_dst) != ntohs(hdr->udp_src)) continue;
        
        uint8_t *rp = rx + sizeof(net_header_t);
        uint32_t rplen = (uint32_t)rlen - sizeof(net_header_t);
        if (rplen < 12) continue;
        
        uint16_t rxid = (rp[0] << 8) | rp[1];
        if (rxid != txid) continue;
        
        uint16_t flags = (rp[2] << 8) | rp[3];
        if ((flags & 0x8000) == 0) continue; /* Not a response */
        if ((flags & 0x000F) != 0) {
            hal_console_printf("[dns] Server returned error %d\n", flags & 0x000F);
            return HAL_ERROR;
        }
        
        uint16_t qcount = (rp[4] << 8) | rp[5];
        uint16_t ancount = (rp[6] << 8) | rp[7];
        if (ancount == 0) {
            hal_console_puts("[dns] No answers found in response\n");
            return HAL_ERROR;
        }

        uint32_t roff = 12;
        /* Skip question section */
        for (int i = 0; i < qcount && roff < rplen; i++) {
            roff = skip_name(rp, rplen, roff);
            roff += 4; /* skip QTYPE/QCLASS */
        }
        
        /* Parse answers */
        for (int i = 0; i < ancount && roff + 10 <= rplen; i++) {
            roff = skip_name(rp, rplen, roff);
            if (roff + 10 > rplen) break;
            uint16_t type = (rp[roff] << 8) | rp[roff+1];
            uint16_t rdlength = (rp[roff+8] << 8) | rp[roff+9];
            roff += 10;
            if (type == 1 && rdlength == 4 && roff + 4 <= rplen) { /* A record */
                *out_ip = (rp[roff] << 24) | (rp[roff+1] << 16) | (rp[roff+2] << 8) | rp[roff+3];
                hal_console_printf("[dns] Resolved to %u.%u.%u.%u\n",
                       (*out_ip >> 24) & 0xFF, (*out_ip >> 16) & 0xFF,
                       (*out_ip >> 8) & 0xFF, *out_ip & 0xFF);
                return HAL_OK;
            }
            roff += rdlength;
        }
        return HAL_ERROR;
    }
    hal_console_puts("[dns] Timeout\n");
    return HAL_TIMEOUT;
}
