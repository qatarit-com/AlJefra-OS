/* SPDX-License-Identifier: MIT */
/* AlJefra OS — USB CDC Ethernet (ECM) Network Driver
 *
 * Supports USB network adapters that implement:
 *   - CDC ECM (Ethernet Control Model) — standard USB Ethernet
 *   - Common vendor-specific adapters (ASIX AX88179, RTL8153)
 *
 * Uses xHCI bulk transfers for data and control transfers for CDC commands.
 */

#ifndef ALJEFRA_DRV_USB_NET_H
#define ALJEFRA_DRV_USB_NET_H

#include "../../hal/hal.h"
#include "../input/xhci.h"

/* Maximum Ethernet frame size */
#define USB_NET_MTU         1514   /* 1500 + 14 header */
#define USB_NET_RX_BUF_SIZE 2048

/* USB Ethernet device state */
typedef struct {
    xhci_controller_t *hc;
    uint8_t            slot_id;

    /* CDC ECM interface numbers */
    uint8_t            ctrl_iface;      /* Control interface number */
    uint8_t            data_iface;      /* Data interface number */

    /* Bulk endpoint info */
    uint8_t            bulk_in_ep;      /* Bulk IN endpoint number */
    uint16_t           bulk_in_maxpkt;
    uint8_t            bulk_out_ep;     /* Bulk OUT endpoint number */
    uint16_t           bulk_out_maxpkt;

    /* MAC address */
    uint8_t            mac[6];

    /* Device identity */
    uint16_t           vendor_id;
    uint16_t           product_id;

    bool               initialized;
    bool               link_up;
} usb_net_dev_t;

/* Initialize a USB Ethernet device on a given xHCI slot.
 * Parses config descriptors to find CDC ECM or known vendor interfaces.
 * Returns HAL_OK if a network adapter was found and configured. */
hal_status_t usb_net_init(usb_net_dev_t *dev, xhci_controller_t *hc,
                           uint8_t slot_id);

/* Send an Ethernet frame. Returns HAL_OK on success. */
hal_status_t usb_net_send(usb_net_dev_t *dev, const void *frame,
                           uint16_t length);

/* Receive an Ethernet frame (polling).
 * buf must be at least USB_NET_RX_BUF_SIZE bytes.
 * *length is set to actual received frame length.
 * Returns HAL_OK if a frame was received, HAL_NO_DEVICE if none. */
hal_status_t usb_net_recv(usb_net_dev_t *dev, void *buf, uint16_t *length);

/* Get MAC address */
void usb_net_get_mac(usb_net_dev_t *dev, uint8_t mac[6]);

/* Inspect the currently bound USB Ethernet adapter, if any. */
int usb_net_is_ready(void);
uint16_t usb_net_vendor_id(void);
uint16_t usb_net_product_id(void);
uint8_t usb_net_slot_id(void);

/* Register the USB network driver (called from kernel/main.c) */
void usb_net_register(void);

#endif /* ALJEFRA_DRV_USB_NET_H */
