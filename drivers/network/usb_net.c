/* SPDX-License-Identifier: MIT */
/* AlJefra OS — USB CDC Ethernet (ECM) Network Driver Implementation
 *
 * Supports USB network adapters via CDC ECM class.
 * Also supports common vendor-specific adapters by VID:PID.
 *
 * CDC ECM overview:
 *   - Control interface (CDC class 0x02, subclass 0x06): management
 *   - Data interface (CDC Data class 0x0A): bulk IN/OUT for Ethernet frames
 *   - Each bulk transfer carries one complete Ethernet frame
 */

#include "usb_net.h"
#include "../../lib/string.h"
#include "../../kernel/driver_loader.h"

/* ── Known USB Ethernet adapter vendor/product IDs ── */

/* ASIX */
#define ASIX_VID        0x0B95
#define AX88179_PID     0x1790    /* USB 3.0 Gigabit */
#define AX88772_PID     0x772B    /* USB 2.0 100Mbps */

/* Realtek */
#define RTL_VID         0x0BDA
#define RTL8153_PID     0x8153    /* USB 3.0 Gigabit */
#define RTL8152_PID     0x8152    /* USB 2.0 100Mbps */

/* ── ASIX AX88179A vendor registers ── */
#define AX_ACCESS_MAC               0x01
#define AX_ACCESS_PHY               0x02
#define AX_PHYPWR_RSTCTL            0x26
#define AX_CLK_SELECT               0x33
#define AX_RXCOE_CTL                0x34
#define AX_TXCOE_CTL                0x35
#define AX_NODE_ID                  0x10
#define AX_MONITOR_MODE             0x24
#define AX_MEDIUM_STATUS_MODE       0x22
#define AX_RX_CTL                   0x0B
#define AX_RX_BULKIN_QCTRL          0x2E
#define AX_PAUSE_WATERLVL_HIGH      0x54
#define AX_PAUSE_WATERLVL_LOW       0x55

#define AX_PHYPWR_RSTCTL_BZ         0x0010
#define AX_PHYPWR_RSTCTL_IPRL       0x0020

#define AX_CLK_SELECT_BCS           0x01
#define AX_CLK_SELECT_ACS           0x04
#define AX_CLK_SELECT_ULR           0x08
#define AX_CLK_SELECT_ACSREQ        0x10

#define AX_MONITOR_MODE_PMETYPE     0x0100
#define AX_MONITOR_MODE_PMEPOL      0x0020
#define AX_MONITOR_MODE_RWMP        0x0004

#define AX_MEDIUM_RECEIVE_EN        0x0100
#define AX_MEDIUM_TXFLOW_CTRLEN     0x0010
#define AX_MEDIUM_RXFLOW_CTRLEN     0x0020
#define AX_MEDIUM_FULL_DUPLEX       0x0002
#define AX_MEDIUM_GIGAMODE          0x0001
#define AX_MEDIUM_EN_125MHZ         0x0008

#define AX_RX_CTL_START             0x00000080u
#define AX_RX_CTL_DROPCRCERR        0x01000000u
#define AX_RX_CTL_IPE               0x02000000u

#define AX_RXCOE_DEF_CSUM           0x017F
#define AX_TXCOE_DEF_CSUM           0x017F

#define AX_MEDIUM_DEFAULT           (AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW_CTRLEN | \
                                     AX_MEDIUM_RXFLOW_CTRLEN | AX_MEDIUM_FULL_DUPLEX | \
                                     AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ)

/* ── CDC ECM descriptor parsing ── */

/* Parse hex digit */
static uint8_t hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

/* Try to get a USB string descriptor and extract ASCII text */
static hal_status_t usb_get_string(xhci_controller_t *hc, uint8_t slot_id,
                                    uint8_t index, char *out, uint8_t max_len)
{
    if (index == 0 || max_len < 2)
        return HAL_ERROR;

    uint8_t buf[128];
    hal_status_t st = xhci_control_transfer(hc, slot_id,
        0x80, USB_REQ_GET_DESCRIPTOR,
        (USB_DESC_STRING << 8) | index,
        0x0409,  /* English US */
        sizeof(buf), buf);

    if (st != HAL_OK)
        return st;

    uint8_t len = buf[0];
    if (len < 2 || buf[1] != USB_DESC_STRING)
        return HAL_ERROR;

    /* Convert UTF-16LE to ASCII */
    uint8_t chars = (len - 2) / 2;
    if (chars >= max_len) chars = max_len - 1;
    for (uint8_t i = 0; i < chars; i++)
        out[i] = (char)buf[2 + i * 2];
    out[chars] = '\0';

    return HAL_OK;
}

/* Parse MAC address from a hex string like "001122334455" */
static void parse_mac_string(const char *s, uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        mac[i] = (hex_digit(s[i * 2]) << 4) | hex_digit(s[i * 2 + 1]);
    }
}

static hal_status_t asix_read_cmd(usb_net_dev_t *dev, uint8_t access,
                                  uint16_t value, uint16_t index,
                                  uint16_t len, void *buf)
{
    return xhci_control_transfer(dev->hc, dev->slot_id,
        0xC0, access, value, index, len, buf);
}

static hal_status_t asix_write_cmd(usb_net_dev_t *dev, uint8_t access,
                                   uint16_t value, uint16_t index,
                                   uint16_t len, const void *buf)
{
    return xhci_control_transfer(dev->hc, dev->slot_id,
        0x40, access, value, index, len, (void *)buf);
}

static hal_status_t asix_write_u16(usb_net_dev_t *dev, uint16_t reg,
                                   uint16_t value)
{
    return asix_write_cmd(dev, AX_ACCESS_MAC, reg, 0, sizeof(value), &value);
}

static hal_status_t asix_write_u32(usb_net_dev_t *dev, uint16_t reg,
                                   uint32_t value)
{
    return asix_write_cmd(dev, AX_ACCESS_MAC, reg, 0, sizeof(value), &value);
}

static hal_status_t asix_read_mac(usb_net_dev_t *dev, uint8_t mac[6])
{
    return asix_read_cmd(dev, AX_ACCESS_MAC, AX_NODE_ID, 0, 6, mac);
}

/* Minimal AX88179A bring-up based on the upstream Linux init sequence. */
static hal_status_t asix_ax88179a_init(usb_net_dev_t *dev)
{
    static const uint8_t bulkin_qctrl[5] = {0x07, 0x4F, 0x00, 0x18, 0x00};
    uint16_t phypwr = AX_PHYPWR_RSTCTL_BZ | AX_PHYPWR_RSTCTL_IPRL;
    uint16_t clk = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
    uint16_t monitor = AX_MONITOR_MODE_PMETYPE |
                       AX_MONITOR_MODE_PMEPOL |
                       AX_MONITOR_MODE_RWMP;
    uint16_t medium = AX_MEDIUM_DEFAULT;
    uint16_t rxcoe = AX_RXCOE_DEF_CSUM;
    uint16_t txcoe = AX_TXCOE_DEF_CSUM;
    uint8_t pause_high = 0x34;
    uint8_t pause_low = 0x52;
    uint32_t rxctl = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE | AX_RX_CTL_START;

    hal_console_puts("[usb-net] AX88179A vendor init\n");

    if (asix_write_u16(dev, AX_PHYPWR_RSTCTL, phypwr) != HAL_OK)
        return HAL_ERROR;
    hal_timer_delay_ms(2);

    if (asix_write_u16(dev, AX_CLK_SELECT, clk) != HAL_OK)
        return HAL_ERROR;
    hal_timer_delay_ms(1);

    if (asix_read_mac(dev, dev->mac) != HAL_OK)
        return HAL_ERROR;

    if (asix_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH, 0,
                       sizeof(pause_high), &pause_high) != HAL_OK)
        return HAL_ERROR;
    if (asix_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 0,
                       sizeof(pause_low), &pause_low) != HAL_OK)
        return HAL_ERROR;
    if (asix_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 0,
                       sizeof(bulkin_qctrl), bulkin_qctrl) != HAL_OK)
        return HAL_ERROR;
    if (asix_write_u16(dev, AX_RXCOE_CTL, rxcoe) != HAL_OK)
        return HAL_ERROR;
    if (asix_write_u16(dev, AX_TXCOE_CTL, txcoe) != HAL_OK)
        return HAL_ERROR;
    if (asix_write_u16(dev, AX_MONITOR_MODE, monitor) != HAL_OK)
        return HAL_ERROR;
    if (asix_write_u16(dev, AX_MEDIUM_STATUS_MODE, medium) != HAL_OK)
        return HAL_ERROR;

    clk = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS |
          AX_CLK_SELECT_ULR | AX_CLK_SELECT_ACSREQ;
    if (asix_write_u16(dev, AX_CLK_SELECT, clk) != HAL_OK)
        return HAL_ERROR;
    hal_timer_delay_ms(1);

    if (asix_write_u32(dev, AX_RX_CTL, rxctl) != HAL_OK)
        return HAL_ERROR;

    return HAL_OK;
}

/* Parse CDC ECM configuration descriptor to find interfaces and endpoints */
static hal_status_t parse_cdc_ecm(usb_net_dev_t *dev, const uint8_t *cfg_buf,
                                   uint16_t total_len)
{
    bool found_ecm = false;
    bool found_data = false;
    uint8_t mac_string_idx = 0;

    uint16_t off = ((usb_config_desc_t *)cfg_buf)->bLength;

    while (off + 2 <= total_len) {
        uint8_t dlen = cfg_buf[off];
        uint8_t dtype = cfg_buf[off + 1];
        if (dlen < 2) break;

        /* Interface descriptor */
        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            usb_interface_desc_t *iface = (usb_interface_desc_t *)&cfg_buf[off];

            /* CDC ECM control interface */
            if (iface->bInterfaceClass == USB_CLASS_CDC &&
                iface->bInterfaceSubClass == USB_CDC_SUBCLASS_ECM) {
                dev->ctrl_iface = iface->bInterfaceNumber;
                found_ecm = true;
                hal_console_printf("[usb-net] Found CDC ECM control iface %u\n",
                                   iface->bInterfaceNumber);
            }

            /* CDC Data interface (carries Ethernet frames) */
            if (iface->bInterfaceClass == USB_CLASS_CDC_DATA &&
                iface->bNumEndpoints >= 2) {
                dev->data_iface = iface->bInterfaceNumber;
                found_data = true;
            }
        }

        /* CDC Ethernet Networking functional descriptor */
        if (dtype == 0x24 && dlen >= 13) {  /* CS_INTERFACE */
            uint8_t subtype = cfg_buf[off + 2];
            if (subtype == USB_CDC_ETHER_TYPE) {
                mac_string_idx = cfg_buf[off + 3];  /* iMACAddress */
            }
        }

        /* Endpoint descriptor — collect bulk endpoints */
        if (dtype == USB_DESC_ENDPOINT && dlen >= 7 && found_data) {
            usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)&cfg_buf[off];
            uint8_t xfer_type = ep->bmAttributes & 0x03;

            if (xfer_type == 2) {  /* Bulk */
                uint8_t ep_num = ep->bEndpointAddress & 0x0F;
                uint16_t maxpkt = ep->wMaxPacketSize;

                if (ep->bEndpointAddress & 0x80) {
                    /* IN endpoint */
                    dev->bulk_in_ep = ep_num;
                    dev->bulk_in_maxpkt = maxpkt;
                } else {
                    /* OUT endpoint */
                    dev->bulk_out_ep = ep_num;
                    dev->bulk_out_maxpkt = maxpkt;
                }
            }
        }

        off += dlen;
    }

    /* Try to read MAC address from string descriptor */
    if (mac_string_idx != 0) {
        char mac_str[16];
        if (usb_get_string(dev->hc, dev->slot_id, mac_string_idx,
                           mac_str, sizeof(mac_str)) == HAL_OK) {
            if (str_len(mac_str) >= 12)
                parse_mac_string(mac_str, dev->mac);
        }
    }

    return (found_ecm || found_data) ? HAL_OK : HAL_ERROR;
}

/* Try vendor-specific init for known adapters */
static hal_status_t try_vendor_adapter(usb_net_dev_t *dev,
                                        const uint8_t *cfg_buf,
                                        uint16_t total_len)
{
    /* ASIX AX88179 / AX88772 — use bulk endpoints directly */
    if (dev->vendor_id == ASIX_VID) {
        hal_console_printf("[usb-net] ASIX adapter detected (PID %04x)\n",
                           dev->product_id);
    }
    /* Realtek RTL8153 / RTL8152 */
    else if (dev->vendor_id == RTL_VID) {
        hal_console_printf("[usb-net] Realtek adapter detected (PID %04x)\n",
                           dev->product_id);
    }
    else {
        return HAL_ERROR;
    }

    /* For vendor adapters, scan config descriptor for bulk endpoints */
    uint16_t off = ((usb_config_desc_t *)cfg_buf)->bLength;
    bool in_data_iface = false;

    while (off + 2 <= total_len) {
        uint8_t dlen = cfg_buf[off];
        uint8_t dtype = cfg_buf[off + 1];
        if (dlen < 2) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            usb_interface_desc_t *iface = (usb_interface_desc_t *)&cfg_buf[off];
            /* Vendor adapters often have vendor-class data interfaces */
            if (iface->bNumEndpoints >= 2) {
                in_data_iface = true;
                dev->data_iface = iface->bInterfaceNumber;
            }
        }

        if (dtype == USB_DESC_ENDPOINT && dlen >= 7 && in_data_iface) {
            usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)&cfg_buf[off];
            uint8_t xfer_type = ep->bmAttributes & 0x03;

            if (xfer_type == 2) {  /* Bulk */
                uint8_t ep_num = ep->bEndpointAddress & 0x0F;
                uint16_t maxpkt = ep->wMaxPacketSize;

                if (ep->bEndpointAddress & 0x80) {
                    dev->bulk_in_ep = ep_num;
                    dev->bulk_in_maxpkt = maxpkt;
                } else {
                    dev->bulk_out_ep = ep_num;
                    dev->bulk_out_maxpkt = maxpkt;
                }
            }
        }

        off += dlen;
    }

    /* Generate a default MAC for vendor adapters (read from device if possible) */
    if (dev->mac[0] == 0 && dev->mac[1] == 0 && dev->mac[2] == 0) {
        /* Try reading MAC via vendor-specific control request */
        uint8_t mac_buf[6] = {0};
        hal_status_t st;

        if (dev->vendor_id == ASIX_VID) {
            st = asix_read_mac(dev, mac_buf);
        } else if (dev->vendor_id == RTL_VID) {
            /* RTL8153: read MAC from PLA registers isn't simple via control,
             * so we generate a locally-administered MAC */
            st = HAL_ERROR;
        } else {
            st = HAL_ERROR;
        }

        if (st == HAL_OK) {
            memcpy(dev->mac, mac_buf, 6);
        } else {
            /* Generate locally-administered MAC from slot ID */
            dev->mac[0] = 0x02;  /* Locally administered */
            dev->mac[1] = 0xAF;  /* "AlJefra" */
            dev->mac[2] = 0x00;
            dev->mac[3] = dev->slot_id;
            dev->mac[4] = (uint8_t)(dev->vendor_id & 0xFF);
            dev->mac[5] = (uint8_t)(dev->product_id & 0xFF);
        }
    }

    return (dev->bulk_in_ep && dev->bulk_out_ep) ? HAL_OK : HAL_ERROR;
}

/* ── Public API ── */

hal_status_t usb_net_init(usb_net_dev_t *dev, xhci_controller_t *hc,
                           uint8_t slot_id)
{
    memset(dev, 0, sizeof(*dev));
    dev->hc = hc;
    dev->slot_id = slot_id;

    /* Read device descriptor */
    xhci_slot_t *slot = &hc->slots[slot_id - 1];
    if (!slot->active)
        return HAL_ERROR;

    usb_device_desc_t *dd = &slot->dev_desc;
    dev->vendor_id = dd->idVendor;
    dev->product_id = dd->idProduct;

    /* Read full configuration descriptor */
    uint8_t cfg_buf[512];
    hal_status_t st = xhci_get_config_desc(hc, slot_id, cfg_buf, sizeof(cfg_buf));
    if (st != HAL_OK)
        return st;

    usb_config_desc_t *cfg = (usb_config_desc_t *)cfg_buf;
    uint16_t total = cfg->wTotalLength;
    if (total > sizeof(cfg_buf)) total = sizeof(cfg_buf);

    /* Try CDC ECM first */
    bool is_cdc = false;
    if (dd->bDeviceClass == USB_CLASS_CDC ||
        dd->bDeviceClass == USB_CLASS_PER_IFACE) {
        if (parse_cdc_ecm(dev, cfg_buf, total) == HAL_OK &&
            dev->bulk_in_ep && dev->bulk_out_ep) {
            is_cdc = true;
        }
    }

    /* Fall back to vendor-specific detection */
    if (!is_cdc) {
        st = try_vendor_adapter(dev, cfg_buf, total);
        if (st != HAL_OK)
            return HAL_NO_DEVICE;
    }

    hal_console_printf("[usb-net] Bulk IN ep%u (%u), OUT ep%u (%u)\n",
                       dev->bulk_in_ep, dev->bulk_in_maxpkt,
                       dev->bulk_out_ep, dev->bulk_out_maxpkt);

    /* Set configuration */
    st = xhci_set_config(hc, slot_id, cfg->bConfigurationValue);
    if (st != HAL_OK) {
        hal_console_puts("[usb-net] Set configuration failed\n");
        return st;
    }

    /* Configure bulk endpoints in xHCI */
    st = xhci_configure_bulk_eps(hc, slot_id,
                                  dev->bulk_in_ep, dev->bulk_in_maxpkt,
                                  dev->bulk_out_ep, dev->bulk_out_maxpkt);
    if (st != HAL_OK) {
        hal_console_puts("[usb-net] Configure bulk endpoints failed\n");
        return st;
    }

    /* For CDC ECM: send SetInterface to activate data interface
     * (alternate setting 1 enables the bulk endpoints) */
    if (is_cdc) {
        xhci_control_transfer(hc, slot_id,
            0x01,  /* Host-to-device, Standard, Interface */
            USB_REQ_SET_INTERFACE,
            1,     /* Alternate Setting 1 */
            dev->data_iface,
            0, NULL);
    } else if (dev->vendor_id == ASIX_VID) {
        st = asix_ax88179a_init(dev);
        if (st != HAL_OK) {
            hal_console_puts("[usb-net] AX88179A init failed\n");
            return st;
        }
    }

    hal_console_printf("[usb-net] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                       dev->mac[0], dev->mac[1], dev->mac[2],
                       dev->mac[3], dev->mac[4], dev->mac[5]);

    dev->link_up = true;
    dev->initialized = true;
    return HAL_OK;
}

hal_status_t usb_net_send(usb_net_dev_t *dev, const void *frame,
                           uint16_t length)
{
    if (!dev->initialized || !dev->link_up)
        return HAL_ERROR;
    if (length > USB_NET_MTU)
        return HAL_ERROR;

    return xhci_bulk_send(dev->hc, dev->slot_id, frame, length);
}

hal_status_t usb_net_recv(usb_net_dev_t *dev, void *buf, uint16_t *length)
{
    if (!dev->initialized || !dev->link_up)
        return HAL_ERROR;

    return xhci_bulk_recv(dev->hc, dev->slot_id,
                           buf, USB_NET_RX_BUF_SIZE, length);
}

void usb_net_get_mac(usb_net_dev_t *dev, uint8_t mac[6])
{
    memcpy(mac, dev->mac, 6);
}

/* ── driver_ops_t registration ── */

static usb_net_dev_t g_usb_nic;
static bool          g_usb_nic_found = false;

int usb_net_is_ready(void)
{
    return g_usb_nic_found && g_usb_nic.initialized;
}

uint16_t usb_net_vendor_id(void)
{
    return g_usb_nic.vendor_id;
}

uint16_t usb_net_product_id(void)
{
    return g_usb_nic.product_id;
}

uint8_t usb_net_slot_id(void)
{
    return g_usb_nic.slot_id;
}

static hal_status_t usb_net_drv_init(hal_device_t *pci_dev)
{
    (void)pci_dev;

    /* This is called after xHCI init. Find a USB network adapter. */
    xhci_controller_t *hc = xhci_get_controller();
    if (!hc)
        return HAL_NO_DEVICE;

    for (uint8_t i = 0; i < hc->max_slots; i++) {
        if (!hc->slots[i].active)
            continue;

        uint8_t sid = hc->slots[i].slot_id;
        usb_device_desc_t *dd = &hc->slots[i].dev_desc;

        if (dd->idVendor == 0 && dd->idProduct == 0)
            xhci_get_device_desc(hc, sid, dd);

        /* Check if this is a potential network adapter:
         * - CDC class (0x02)
         * - Per-interface class (0x00) — need to check interfaces
         * - Known vendor IDs */
        bool maybe_net = (dd->bDeviceClass == USB_CLASS_CDC) ||
                         (dd->bDeviceClass == USB_CLASS_PER_IFACE) ||
                         (dd->idVendor == ASIX_VID) ||
                         (dd->idVendor == RTL_VID);

        if (!maybe_net)
            continue;

        if (usb_net_init(&g_usb_nic, hc, sid) == HAL_OK) {
            hal_console_printf("[usb-net] USB Ethernet ready on slot %u\n", sid);
            g_usb_nic_found = true;
            return HAL_OK;
        }
    }

    hal_console_puts("[usb-net] No USB network adapter found\n");
    return HAL_NO_DEVICE;
}

static void usb_net_drv_shutdown(void)
{
    g_usb_nic_found = false;
    g_usb_nic.initialized = false;
}

static int64_t usb_net_drv_tx(const void *frame, uint64_t len)
{
    if (!g_usb_nic_found || len > 0xFFFF)
        return -1;
    hal_status_t st = usb_net_send(&g_usb_nic, frame, (uint16_t)len);
    return (st == HAL_OK) ? (int64_t)len : -1;
}

static int64_t usb_net_drv_rx(void *frame, uint64_t max_len)
{
    if (!g_usb_nic_found || max_len < USB_NET_RX_BUF_SIZE)
        return -1;
    uint16_t length = 0;
    hal_status_t st = usb_net_recv(&g_usb_nic, frame, &length);
    return (st == HAL_OK) ? (int64_t)length : -1;
}

static void usb_net_drv_get_mac(uint8_t mac[6])
{
    if (g_usb_nic_found)
        usb_net_get_mac(&g_usb_nic, mac);
    else
        memset(mac, 0, 6);
}

static const driver_ops_t usb_net_driver_ops = {
    .name        = "usb-net",
    .category    = DRIVER_CAT_NETWORK,
    .init        = usb_net_drv_init,
    .shutdown    = usb_net_drv_shutdown,
    .net_tx      = usb_net_drv_tx,
    .net_rx      = usb_net_drv_rx,
    .net_get_mac = usb_net_drv_get_mac,
};

void usb_net_register(void)
{
    driver_register_builtin(&usb_net_driver_ops);
}
