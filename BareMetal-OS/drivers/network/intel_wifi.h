/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Intel AX200/AX210 WiFi Driver
 * Architecture-independent; uses HAL for all hardware access.
 *
 * Supports:
 *   - Intel Wi-Fi 6 AX200 (PCI ID 8086:2723)
 *   - Intel Wi-Fi 6E AX210 (PCI ID 8086:2725)
 *
 * The Intel WiFi hardware (iwlwifi family) uses a command/response
 * architecture with TFD (Transmit Frame Descriptor) rings for commands
 * and data, and RBD (Receive Buffer Descriptor) rings for responses
 * and received frames.
 *
 * Firmware is required and must be loaded before the device becomes
 * operational. The driver parses IWL firmware format (.ucode files).
 */

#ifndef ALJEFRA_DRV_INTEL_WIFI_H
#define ALJEFRA_DRV_INTEL_WIFI_H

#include "../../hal/hal.h"
#include "wifi_framework.h"

/* ====================================================================
 * Device IDs
 * ==================================================================== */

#define INTEL_VENDOR_ID        0x8086
#define INTEL_AX200_DEVICE_ID  0x2723
#define INTEL_AX210_DEVICE_ID  0x2725

/* ====================================================================
 * Register Definitions (iwlwifi CSR)
 * ==================================================================== */

/* Hardware Revision */
#define IWL_CSR_HW_REV          0x028

/* Hardware IF config */
#define IWL_CSR_HW_IF_CONFIG    0x000

/* Interrupt registers */
#define IWL_CSR_INT             0x008
#define IWL_CSR_INT_MASK        0x00C
#define IWL_CSR_FH_INT          0x010

/* Reset control */
#define IWL_CSR_RESET           0x020

/* GP (General Purpose) control */
#define IWL_CSR_GP_CNTRL        0x024
#define IWL_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY  (1u << 0)
#define IWL_CSR_GP_CNTRL_REG_FLAG_INIT_DONE        (1u << 2)
#define IWL_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ   (1u << 3)
#define IWL_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP   (1u << 4)
#define IWL_CSR_GP_CNTRL_REG_FLAG_XTAL_ON          (1u << 15)
#define IWL_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN     (1u << 0)

/* EEPROM / OTP / NVM */
#define IWL_CSR_EEPROM_GP       0x030
#define IWL_CSR_OTP_GP          0x034
#define IWL_CSR_GIO             0x03C
#define IWL_CSR_GP_UCODE        0x048

/* TFD (Transmit Frame Descriptor) queue base registers */
#define IWL_FH_MEM_CBBC_BASE   0x19000  /* TFD circular buffer base */
#define IWL_FH_MEM_CBBC(q)     (IWL_FH_MEM_CBBC_BASE + 4 * (q))

/* TX scheduler */
#define IWL_SCD_BASE            0x0A02C00
#define IWL_SCD_TXFACT          0x0A02C1C
#define IWL_SCD_QUEUECHAIN_SEL  0x0A02CE0
#define IWL_SCD_AGGR_SEL        0x0A02CE4
#define IWL_SCD_QUEUE_STATUS(q) (0x0A02C00 + 0x104 + 4 * (q))

/* FH (Firmware-Hardware interface) TX config per channel */
#define IWL_FH_TCSR_BASE        0x1D00
#define IWL_FH_TCSR_CONFIG(ch)  (IWL_FH_TCSR_BASE + 0x20 * (ch))
#define IWL_FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE (1u << 31)

/* FH RX config */
#define IWL_FH_RX_CONFIG        0x1C00
#define IWL_FH_RSCSR_CHNL0_RBDC_BASE  0x1C80
#define IWL_FH_RSCSR_CHNL0_STTS_WPTR  0x1C90

/* FH RX status */
#define IWL_FH_MEM_RSCSR_BASE   0x1BC0

/* Peripheral (PRPH) access */
#define IWL_CSR_PRPH_WADDR      0x044
#define IWL_CSR_PRPH_RADDR      0x048
#define IWL_CSR_PRPH_WDATA      0x04C
#define IWL_CSR_PRPH_RDATA      0x050

/* MAC/PHY internal */
#define IWL_CSR_DRAM_INT_TBL_REG  0x00A

/* Power */
#define IWL_CSR_APMG_CLK_EN     0x300000
#define IWL_CSR_APMG_PS         0x300004

/* DBG registers */
#define IWL_CSR_DBG_HPET_MEM    0x240

/* ====================================================================
 * Firmware Image Constants
 * ==================================================================== */

/* IWL ucode file magic */
#define IWL_UCODE_MAGIC         0x0A4C5749  /* "IWL\n" */

/* Firmware section types */
#define IWL_UCODE_TLV_INST      1   /* Instruction text */
#define IWL_UCODE_TLV_DATA      2   /* Data */
#define IWL_UCODE_TLV_INIT      3   /* Init instruction text */
#define IWL_UCODE_TLV_INIT_DATA 4   /* Init data */
#define IWL_UCODE_TLV_BOOT      5   /* Bootstrap */
#define IWL_UCODE_TLV_PROBE_MAX 6   /* Max probe length */
#define IWL_UCODE_TLV_CMD_VER   48  /* Command versions */
#define IWL_UCODE_TLV_PHY_CFG   72  /* PHY configuration */

/* Firmware TLV header */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t length;
    /* data follows */
} iwl_ucode_tlv_t;

/* Firmware file header */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t ver;
    uint32_t size;       /* Size of data after this header */
    /* TLVs follow */
} iwl_ucode_hdr_t;

/* ====================================================================
 * DMA Descriptors
 * ==================================================================== */

/* TFD (Transmit Frame Descriptor) -- 128 bytes */
#define IWL_NUM_TFD_TBS     20   /* Max transfer buffers per TFD */

typedef struct __attribute__((packed)) {
    uint32_t tb_lo;     /* DMA address low 32 bits */
    uint16_t tb_hi_n_len; /* High 4 bits of addr + length */
} iwl_tfd_tb_t;

typedef struct __attribute__((packed)) {
    uint8_t     __reserved1[3];
    uint8_t     num_tbs;          /* Number of transfer buffers */
    iwl_tfd_tb_t tbs[IWL_NUM_TFD_TBS];
    uint32_t    __pad;
} iwl_tfd_t;

/* RBD (Receive Buffer Descriptor) -- 8 bytes */
typedef struct __attribute__((packed)) {
    uint32_t addr_lo;
    uint32_t addr_hi;
} iwl_rbd_t;

/* RB Status -- 4 bytes */
typedef struct __attribute__((packed)) {
    uint16_t closed_rb_num;
    uint16_t closed_fr_num;
} iwl_rb_status_t;

/* ====================================================================
 * Host Command Interface
 * ==================================================================== */

/* Host command header (sent via TFD queue 0 or 4) */
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint8_t  group_id;
    uint8_t  seq;           /* Sequence number (set by driver) */
    uint8_t  flags;
    uint16_t length;        /* Data length after this header */
    uint8_t  reserved[2];
    /* Command data follows */
} iwl_host_cmd_hdr_t;

/* Common host command IDs */
#define IWL_CMD_ALIVE           0x01
#define IWL_CMD_PHY_CONFIG      0x6A
#define IWL_CMD_NVM_ACCESS      0x88
#define IWL_CMD_PHY_CONTEXT     0x08
#define IWL_CMD_BINDING         0x2B
#define IWL_CMD_MAC_CONTEXT     0x28
#define IWL_CMD_TIME_EVENT      0x29
#define IWL_CMD_SCAN_REQ        0x80
#define IWL_CMD_SCAN_ABORT      0x81
#define IWL_CMD_SCAN_COMPLETE   0x84
#define IWL_CMD_TX              0x1C
#define IWL_CMD_TXPOWER         0xC5
#define IWL_CMD_ADD_STA         0x18
#define IWL_CMD_REMOVE_STA      0x19

/* NVM (Non-Volatile Memory) access for MAC address etc. */
#define IWL_NVM_SECTION_HW      0
#define IWL_NVM_SECTION_MAC     1
#define IWL_NVM_SECTION_PHY     2
#define IWL_NVM_SECTION_CAL     4

/* ====================================================================
 * Ring Sizes
 * ==================================================================== */

#define IWL_CMD_QUEUE           4     /* Queue index for host commands */
#define IWL_CMD_QUEUE_SIZE      32    /* TFDs in command queue */
#define IWL_TX_QUEUE            0     /* Queue index for TX data */
#define IWL_TX_QUEUE_SIZE       32    /* TFDs in TX data queue */
#define IWL_RX_RING_SIZE        64    /* Number of RBDs */
#define IWL_RX_BUF_SIZE         4096  /* Bytes per RX buffer */
#define IWL_CMD_BUF_SIZE        320   /* Host command buffer size */
#define IWL_MAX_QUEUES          32    /* Maximum TFD queues */

/* ====================================================================
 * Device State
 * ==================================================================== */

typedef enum {
    IWL_DEV_RESET = 0,
    IWL_DEV_INIT,
    IWL_DEV_FW_LOADING,
    IWL_DEV_FW_ALIVE,
    IWL_DEV_OPERATIONAL,
    IWL_DEV_ERROR,
} iwl_dev_state_t;

typedef struct {
    hal_device_t       dev;
    volatile void     *regs;            /* BAR0 MMIO base */
    uint16_t           device_id;       /* AX200 or AX210 */
    uint32_t           hw_rev;
    iwl_dev_state_t    state;

    /* MAC address from NVM */
    uint8_t            mac[6];

    /* Command queue (TFD ring) */
    iwl_tfd_t         *cmd_tfd;         /* TFD ring (DMA) */
    uint64_t           cmd_tfd_phys;
    void              *cmd_bufs[IWL_CMD_QUEUE_SIZE]; /* Command buffers */
    uint64_t           cmd_bufs_phys[IWL_CMD_QUEUE_SIZE];
    uint16_t           cmd_write;       /* Write pointer */
    uint16_t           cmd_read;        /* Read pointer */
    uint8_t            cmd_seq;         /* Sequence counter */

    /* TX data queue (TFD ring) */
    iwl_tfd_t         *tx_tfd;
    uint64_t           tx_tfd_phys;
    void              *tx_bufs[IWL_TX_QUEUE_SIZE];
    uint64_t           tx_bufs_phys[IWL_TX_QUEUE_SIZE];
    uint16_t           tx_write;
    uint16_t           tx_read;

    /* RX ring (RBD ring) */
    iwl_rbd_t         *rx_rbd;          /* RBD ring (DMA) */
    uint64_t           rx_rbd_phys;
    void              *rx_bufs[IWL_RX_RING_SIZE]; /* RX buffers */
    uint64_t           rx_bufs_phys[IWL_RX_RING_SIZE];
    iwl_rb_status_t   *rx_status;       /* RB status (DMA) */
    uint64_t           rx_status_phys;
    uint16_t           rx_read;         /* Read pointer */

    /* Firmware image (pointer to loaded firmware in memory) */
    const void        *fw_data;
    uint32_t           fw_size;
    uint32_t           fw_inst_size;
    uint32_t           fw_data_size;

    /* WiFi framework context */
    wifi_ctx_t         wifi_ctx;

    /* Current channel */
    uint8_t            channel;

    bool               initialized;
} iwl_dev_t;

/* ====================================================================
 * Public API
 * ==================================================================== */

/* Initialize the Intel WiFi device from a HAL device.
 * Performs:
 *   - PCIe enable + BAR mapping
 *   - NIC reset
 *   - DMA ring allocation
 *   - Firmware loading (if fw_data/fw_size set beforehand)
 *   - NVM read (MAC address)
 *   - WiFi framework initialization
 */
hal_status_t iwl_init(iwl_dev_t *nic, hal_device_t *dev);

/* Load firmware into the device.
 * fw_data: Pointer to IWL firmware image in memory
 * fw_size: Size of firmware image
 *
 * The firmware must be loaded before the device can scan/connect.
 */
hal_status_t iwl_load_firmware(iwl_dev_t *nic, const void *fw_data,
                                uint32_t fw_size);

/* Send a host command to the firmware.
 * cmd_id:   Command ID
 * data:     Command payload
 * data_len: Payload length
 *
 * Returns HAL_OK when the command has been submitted.
 */
hal_status_t iwl_send_cmd(iwl_dev_t *nic, uint8_t cmd_id,
                            const void *data, uint16_t data_len);

/* Wait for a firmware response/notification.
 * buf:      Output buffer for response data
 * buf_size: Maximum response size
 * timeout_ms: Timeout in milliseconds
 *
 * Returns HAL_OK if response received, HAL_TIMEOUT otherwise.
 */
hal_status_t iwl_wait_response(iwl_dev_t *nic, void *buf,
                                 uint32_t buf_size, uint32_t timeout_ms);

/* Send a raw 802.11 frame via the TX data queue */
hal_status_t iwl_tx_raw(iwl_dev_t *nic, const void *frame, uint32_t len);

/* Receive a raw 802.11 frame from the RX ring (polling) */
hal_status_t iwl_rx_raw(iwl_dev_t *nic, void *buf, uint32_t *len);

/* Set radio channel */
hal_status_t iwl_set_channel(iwl_dev_t *nic, uint8_t channel);

/* Read MAC address from NVM */
hal_status_t iwl_read_mac(iwl_dev_t *nic);

/* Get MAC address */
void iwl_get_mac(iwl_dev_t *nic, uint8_t mac[6]);

/* Check if device is operational */
bool iwl_is_operational(iwl_dev_t *nic);

/* Reset the NIC */
hal_status_t iwl_reset(iwl_dev_t *nic);

/* Power management: put device to sleep / wake */
hal_status_t iwl_power_save(iwl_dev_t *nic, bool enable);

/* ---- WiFi operations (high-level) ---- */

/* Scan for networks */
uint32_t iwl_scan(iwl_dev_t *nic, const uint8_t *channels,
                    uint32_t n_chan, uint32_t dwell_ms);

/* Connect to WPA2-PSK network */
hal_status_t iwl_connect(iwl_dev_t *nic, const char *ssid,
                           const char *passphrase);

/* Disconnect */
hal_status_t iwl_disconnect(iwl_dev_t *nic);

/* Send Ethernet frame (auto-converts to 802.11 + encrypts) */
hal_status_t iwl_send(iwl_dev_t *nic, const void *frame, uint32_t len);

/* Receive Ethernet frame (auto-decrypts + converts from 802.11) */
hal_status_t iwl_recv(iwl_dev_t *nic, void *buf, uint32_t *len);

/* ---- driver_ops_t registration ---- */

/* Register this driver with the kernel driver loader */
void intel_wifi_register(void);

#endif /* ALJEFRA_DRV_INTEL_WIFI_H */
