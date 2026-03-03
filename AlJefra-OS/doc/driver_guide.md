# AlJefra OS -- Driver Development Guide

## Overview

AlJefra OS drivers are portable C modules that use the HAL interface for all hardware access. A driver never contains inline assembly -- all hardware interaction goes through `hal_mmio_*`, `hal_bus_*`, `hal_irq_*`, and other HAL functions. This means a single driver source file compiles and runs identically on x86-64, ARM64, and RISC-V 64.

This guide covers the driver model, step-by-step driver development, HAL usage patterns, the runtime driver format, testing, and best practices.

---

## Driver Model

### driver_ops_t Structure

Every driver exposes a `driver_ops_t` structure that the kernel uses to initialize, operate, and shut down the driver.

```c
typedef enum {
    DRIVER_STORAGE  = 0,    /* Block storage devices           */
    DRIVER_NETWORK  = 1,    /* Network interface cards          */
    DRIVER_INPUT    = 2,    /* Keyboards, mice, touchscreens    */
    DRIVER_DISPLAY  = 3,    /* GPUs, framebuffers               */
    DRIVER_BUS      = 4     /* Bus controllers (PCI, USB, ...)  */
} driver_category_t;

typedef struct {
    /* --- Common fields (all drivers) --- */
    const char         *name;         /* Human-readable name, e.g. "e1000"    */
    driver_category_t   category;     /* One of the categories above          */
    hal_status_t      (*init)(void);  /* Called once when the driver is loaded */
    void              (*shutdown)(void); /* Called when the driver is unloaded */

    /* --- Category-specific operations --- */
    union {
        /* DRIVER_STORAGE */
        struct {
            int      (*read)(uint64_t lba, uint32_t count, void *buf);
            int      (*write)(uint64_t lba, uint32_t count, const void *buf);
            uint64_t (*capacity)(void);   /* Total sectors */
        } storage;

        /* DRIVER_NETWORK */
        struct {
            int  (*send)(const void *pkt, uint32_t len);
            int  (*recv)(void *buf, uint32_t max_len);
            void (*get_mac)(uint8_t mac[6]);
        } network;

        /* DRIVER_INPUT */
        struct {
            int (*poll)(void *event);     /* Non-blocking event poll */
        } input;

        /* DRIVER_DISPLAY */
        struct {
            int   (*set_mode)(uint32_t width, uint32_t height, uint32_t bpp);
            void *(*get_framebuffer)(void);
            void  (*flip)(void);          /* Swap front/back buffer */
        } display;

        /* DRIVER_BUS */
        struct {
            int (*scan)(void (*callback)(hal_device_t *dev));
        } bus;
    } ops;
} driver_ops_t;
```

### Driver Categories

| Category | Purpose | Key Operations |
|----------|---------|----------------|
| `DRIVER_STORAGE` | Block devices (disks, SSDs, flash) | `read()`, `write()`, `capacity()` |
| `DRIVER_NETWORK` | Network interfaces | `send()`, `recv()`, `get_mac()` |
| `DRIVER_INPUT` | Human input devices | `poll()` |
| `DRIVER_DISPLAY` | Display adapters | `set_mode()`, `get_framebuffer()`, `flip()` |
| `DRIVER_BUS` | Bus controllers | `scan()` |

---

## Writing a Driver: Step by Step

### Step 1: Create the Source File

Create a new `.c` file in the appropriate subdirectory of `drivers/`:

```
drivers/
  |-- storage/    for DRIVER_STORAGE
  |-- net/        for DRIVER_NETWORK
  |-- input/      for DRIVER_INPUT
  |-- gpu/        for DRIVER_DISPLAY
  +-- bus/        for DRIVER_BUS
```

### Step 2: Include HAL Headers

Every driver includes the master HAL header:

```c
#include "hal/hal.h"
```

This gives you access to all HAL functions: MMIO, interrupts, bus scan, DMA, timers, and console output.

### Step 3: Define Device Constants

Identify the device by its PCI vendor and device IDs (or USB vendor/product IDs):

```c
#define E1000_VENDOR_ID   0x8086
#define E1000_DEVICE_ID   0x100E
```

### Step 4: Define Driver State

Use a static struct to hold per-device state:

```c
typedef struct {
    volatile void *mmio_base;    /* MMIO base address (BAR0)         */
    uint8_t        mac[6];       /* MAC address                      */
    uint8_t        irq;          /* Assigned IRQ                     */
    void          *rx_ring;      /* Receive descriptor ring (DMA)    */
    void          *tx_ring;      /* Transmit descriptor ring (DMA)   */
    uint64_t       rx_ring_phys; /* Physical address of RX ring      */
    uint64_t       tx_ring_phys; /* Physical address of TX ring      */
} e1000_state_t;

static e1000_state_t state;
```

### Step 5: Implement init()

The `init()` function is called once when the driver is loaded. It should:

1. Find the device via bus scan.
2. Map the device's MMIO region.
3. Allocate DMA buffers.
4. Register an interrupt handler.
5. Configure the device hardware.

```c
static hal_status_t e1000_init(void) {
    /* Find the device on the PCI bus */
    hal_device_t dev;
    if (find_pci_device(E1000_VENDOR_ID, E1000_DEVICE_ID, &dev) != HAL_OK) {
        return HAL_NO_DEVICE;
    }

    /* Map BAR0 (MMIO registers) */
    state.mmio_base = (volatile void *)dev.bar[0];
    state.irq = dev.irq;

    /* Enable PCI bus mastering (required for DMA) */
    uint32_t cmd = hal_bus_read_config(dev.bus, dev.dev, dev.func, 0x04, 2);
    cmd |= (1 << 2);   /* Bus Master Enable */
    cmd |= (1 << 1);   /* Memory Space Enable */
    hal_bus_write_config(dev.bus, dev.dev, dev.func, 0x04, 2, cmd);

    /* Allocate DMA ring buffers */
    state.rx_ring = hal_dma_alloc(4096, &state.rx_ring_phys);
    state.tx_ring = hal_dma_alloc(4096, &state.tx_ring_phys);
    if (!state.rx_ring || !state.tx_ring) {
        return HAL_NO_MEMORY;
    }

    /* Register interrupt handler */
    hal_irq_register(state.irq, e1000_irq_handler, &state);
    hal_irq_enable(state.irq);

    /* Read MAC address from EEPROM */
    e1000_read_mac(state.mmio_base, state.mac);

    /* Configure RX and TX rings */
    hal_mmio_write32(state.mmio_base + E1000_RDBAL, (uint32_t)state.rx_ring_phys);
    hal_mmio_write32(state.mmio_base + E1000_RDBAH, (uint32_t)(state.rx_ring_phys >> 32));
    hal_mmio_write32(state.mmio_base + E1000_RDLEN, 4096);

    hal_mmio_write32(state.mmio_base + E1000_TDBAL, (uint32_t)state.tx_ring_phys);
    hal_mmio_write32(state.mmio_base + E1000_TDBAH, (uint32_t)(state.tx_ring_phys >> 32));
    hal_mmio_write32(state.mmio_base + E1000_TDLEN, 4096);

    /* Enable RX and TX */
    hal_mmio_write32(state.mmio_base + E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM);
    hal_mmio_write32(state.mmio_base + E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP);

    hal_console_printf("[e1000] Initialized: MAC=%02x:%02x:%02x:%02x:%02x:%02x IRQ=%u\n",
                       state.mac[0], state.mac[1], state.mac[2],
                       state.mac[3], state.mac[4], state.mac[5],
                       state.irq);

    return HAL_OK;
}
```

### Step 6: Implement Category-Specific Operations

For a network driver, implement `send()`, `recv()`, and `get_mac()`:

```c
static int e1000_send(const void *pkt, uint32_t len) {
    /* Set up TX descriptor with packet pointer and length */
    /* Signal the hardware by writing the TX tail register */
    hal_mmio_write32(state.mmio_base + E1000_TDT, next_tx_index);
    return 0;
}

static int e1000_recv(void *buf, uint32_t max_len) {
    /* Check RX descriptor ring for completed receives */
    /* Copy packet data to buf, up to max_len */
    /* Advance RX tail */
    hal_mmio_write32(state.mmio_base + E1000_RDT, next_rx_index);
    return bytes_received;
}

static void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++)
        mac[i] = state.mac[i];
}
```

### Step 7: Implement the Interrupt Handler

```c
static void e1000_irq_handler(uint32_t irq, void *data) {
    uint32_t icr = hal_mmio_read32(state.mmio_base + E1000_ICR);

    if (icr & E1000_ICR_RXT0) {
        /* Packet received -- wake up any waiting reader */
    }

    if (icr & E1000_ICR_TXDW) {
        /* Transmit complete -- free TX buffer */
    }

    hal_irq_eoi(irq);
}
```

### Step 8: Implement shutdown()

Clean teardown: disable hardware, free DMA buffers, unregister interrupts.

```c
static void e1000_shutdown(void) {
    /* Disable RX and TX */
    hal_mmio_write32(state.mmio_base + E1000_RCTL, 0);
    hal_mmio_write32(state.mmio_base + E1000_TCTL, 0);

    /* Disable interrupts */
    hal_irq_disable(state.irq);
    hal_irq_unregister(state.irq);

    /* Free DMA buffers */
    hal_dma_free(state.rx_ring, 4096);
    hal_dma_free(state.tx_ring, 4096);

    hal_console_printf("[e1000] Shut down.\n");
}
```

### Step 9: Export the driver_ops_t

```c
driver_ops_t e1000_driver = {
    .name     = "e1000",
    .category = DRIVER_NETWORK,
    .init     = e1000_init,
    .shutdown = e1000_shutdown,
    .ops.network = {
        .send    = e1000_send,
        .recv    = e1000_recv,
        .get_mac = e1000_get_mac,
    },
};
```

---

## HAL Usage Patterns

### Bus Scan and Device Discovery

```c
static hal_device_t found_dev;
static int found = 0;

static void scan_callback(hal_device_t *dev) {
    if (dev->vendor_id == MY_VENDOR && dev->device_id == MY_DEVICE) {
        found_dev = *dev;
        found = 1;
    }
}

hal_status_t find_my_device(void) {
    found = 0;
    hal_bus_scan(scan_callback);
    return found ? HAL_OK : HAL_NO_DEVICE;
}
```

### MMIO Register Access

```c
/* Read a 32-bit device register */
uint32_t val = hal_mmio_read32(mmio_base + REG_OFFSET);

/* Write a 32-bit device register */
hal_mmio_write32(mmio_base + REG_OFFSET, new_value);

/* Read-modify-write pattern */
uint32_t ctrl = hal_mmio_read32(mmio_base + REG_CTRL);
ctrl |= CTRL_ENABLE_BIT;
hal_mmio_write32(mmio_base + REG_CTRL, ctrl);
```

### Interrupt Registration

```c
/* Register handler */
hal_irq_register(dev.irq, my_handler, &my_state);
hal_irq_enable(dev.irq);

/* In the handler */
static void my_handler(uint32_t irq, void *data) {
    my_state_t *s = (my_state_t *)data;
    /* Handle interrupt */
    hal_irq_eoi(irq);
}

/* Teardown */
hal_irq_disable(dev.irq);
hal_irq_unregister(dev.irq);
```

### DMA Buffer Allocation

```c
uint64_t phys_addr;
void *buf = hal_dma_alloc(size, &phys_addr);
if (!buf) {
    return HAL_NO_MEMORY;
}

/* Tell the device the physical address */
hal_mmio_write64(mmio_base + REG_DMA_ADDR, phys_addr);

/* Free when done */
hal_dma_free(buf, size);
```

### Timed Polling

```c
/* Poll a status register with timeout */
uint64_t start = hal_timer_ticks();
uint32_t timeout_ticks = 1000;  /* 1 second at 1000 Hz */

while (!(hal_mmio_read32(mmio_base + REG_STATUS) & STATUS_READY)) {
    if (hal_timer_ticks() - start > timeout_ticks) {
        return HAL_TIMEOUT;
    }
}
```

---

## Runtime Drivers: .ajdrv Format

Drivers can be distributed as `.ajdrv` packages that the AI bootstrap downloads and loads at runtime.

### Package Structure

An `.ajdrv` file is a simple archive containing:

```
my_driver.ajdrv
|-- manifest.json      Driver metadata
|-- driver.bin         Compiled driver binary (relocatable)
+-- signature.ed25519  Ed25519 signature of driver.bin
```

### manifest.json

```json
{
  "name": "my-custom-nic",
  "version": "1.0.0",
  "category": "network",
  "author": "Your Name",
  "license": "MIT",
  "description": "Driver for My Custom NIC",
  "supported_archs": ["x86_64", "aarch64", "riscv64"],
  "devices": [
    { "bus": "pci", "vendor_id": "0x1234", "device_id": "0x5678" }
  ],
  "entry_symbol": "my_nic_driver"
}
```

### Building a .ajdrv Package

Use the provided `build_ajdrv.sh` script:

```bash
# Build for all architectures
./build_ajdrv.sh my_driver.c my-custom-nic

# Build for a specific architecture
./build_ajdrv.sh my_driver.c my-custom-nic x86_64
```

The script:

1. Cross-compiles the driver for each target architecture with `-fPIC -nostdlib`.
2. Links against the HAL stubs to produce a relocatable binary.
3. Creates the `manifest.json` from arguments and source annotations.
4. Packages everything into a `.ajdrv` archive.
5. Signs the binary with your developer key (if configured).

### Loading at Runtime

The kernel's driver loader (`kernel/drv_loader.c`):

1. Reads the `.ajdrv` archive from memory (downloaded by the AI agent).
2. Parses `manifest.json` to get the entry symbol name.
3. Maps `driver.bin` into kernel address space.
4. Performs symbol relocation (resolves HAL function addresses).
5. Looks up the `driver_ops_t` by the `entry_symbol`.
6. Calls `driver_ops_t.init()`.

---

## Testing

### QEMU with Emulated Devices

QEMU provides emulated versions of many devices. Use `-device` flags to add devices:

```bash
# Test an e1000 NIC driver
qemu-system-x86_64 \
  -drive file=build/aljefra-x86_64.img,format=raw \
  -m 256M \
  -device e1000,netdev=net0 \
  -netdev user,id=net0 \
  -serial stdio

# Test an NVMe storage driver
qemu-system-x86_64 \
  -drive file=build/aljefra-x86_64.img,format=raw \
  -m 256M \
  -drive file=test-disk.img,if=none,id=nvm \
  -device nvme,serial=deadbeef,drive=nvm \
  -serial stdio

# Test a VirtIO-GPU display driver
qemu-system-x86_64 \
  -drive file=build/aljefra-x86_64.img,format=raw \
  -m 256M \
  -device virtio-gpu-pci \
  -serial stdio

# Test with PCI device passthrough (requires VFIO)
qemu-system-x86_64 \
  -drive file=build/aljefra-x86_64.img,format=raw \
  -m 256M \
  -device vfio-pci,host=01:00.0 \
  -serial stdio
```

### Debug Output

Use `hal_console_printf()` for debug output. All console output goes to both the screen and serial port, so you can capture it in your terminal when using `-serial stdio`.

### Test Harness

The `test/` directory contains test infrastructure. To add tests for your driver:

1. Create `test/test_my_driver.c`.
2. Use the test macros: `TEST_ASSERT(condition)`, `TEST_PASS()`, `TEST_FAIL(msg)`.
3. Add your test to `test/Makefile`.
4. Run: `make test ARCH=x86_64`.

---

## Existing Drivers

AlJefra OS ships with 22+ built-in drivers. Use these as reference implementations.

### Storage Drivers

| Driver | File | Vendor ID | Device ID | Description |
|--------|------|-----------|-----------|-------------|
| NVMe | `drivers/storage/nvme.c` | Various | Various | NVM Express SSD controller |
| AHCI | `drivers/storage/ahci.c` | `0x8086` | Various | SATA/AHCI controller |
| VirtIO-blk | `drivers/storage/virtio_blk.c` | `0x1AF4` | `0x1001` | VirtIO block device |
| IDE/ATA | `drivers/storage/ide.c` | N/A | N/A | Legacy IDE controller (port I/O) |
| Floppy | `drivers/storage/floppy.c` | N/A | N/A | Legacy floppy controller |

### Network Drivers

| Driver | File | Vendor ID | Device ID | Description |
|--------|------|-----------|-----------|-------------|
| e1000 | `drivers/net/e1000.c` | `0x8086` | `0x100E` | Intel Gigabit Ethernet |
| e1000e | `drivers/net/e1000e.c` | `0x8086` | `0x10D3` | Intel Gigabit Ethernet (newer) |
| VirtIO-net | `drivers/net/virtio_net.c` | `0x1AF4` | `0x1000` | VirtIO network device |
| RTL8139 | `drivers/net/rtl8139.c` | `0x10EC` | `0x8139` | Realtek 100 Mbps Ethernet |
| RTL8169 | `drivers/net/rtl8169.c` | `0x10EC` | `0x8169` | Realtek Gigabit Ethernet |

### GPU / Display Drivers

| Driver | File | Vendor ID | Device ID | Description |
|--------|------|-----------|-----------|-------------|
| VirtIO-GPU | `drivers/gpu/virtio_gpu.c` | `0x1AF4` | `0x1050` | VirtIO GPU device |
| Bochs VBE | `drivers/gpu/bochs_vbe.c` | `0x1234` | `0x1111` | Bochs/QEMU VGA extensions |
| VESA FB | `drivers/gpu/vesa_fb.c` | N/A | N/A | VESA framebuffer (generic) |
| VGA Text | `drivers/gpu/vga_text.c` | N/A | N/A | VGA text mode (x86-64 only) |

### Input Drivers

| Driver | File | Vendor ID | Device ID | Description |
|--------|------|-----------|-----------|-------------|
| PS/2 Keyboard | `drivers/input/ps2_kbd.c` | N/A | N/A | PS/2 keyboard controller |
| PS/2 Mouse | `drivers/input/ps2_mouse.c` | N/A | N/A | PS/2 mouse controller |
| USB HID | `drivers/input/usb_hid.c` | Various | Various | USB keyboard and mouse |

### Bus Drivers

| Driver | File | Vendor ID | Device ID | Description |
|--------|------|-----------|-----------|-------------|
| PCI | `drivers/bus/pci.c` | N/A | N/A | PCI bus enumeration |
| xHCI | `drivers/bus/xhci.c` | Various | Various | USB 3.0 host controller |
| EHCI | `drivers/bus/ehci.c` | Various | Various | USB 2.0 host controller |
| UHCI | `drivers/bus/uhci.c` | `0x8086` | Various | USB 1.x host controller |

---

## Best Practices

### 1. Use HAL Only -- No Inline Assembly

Never write inline assembly in a driver. Use HAL functions instead:

```c
/* WRONG -- breaks portability */
asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));

/* CORRECT -- works on all architectures */
hal_io_outb(port, val);
```

For MMIO access:

```c
/* WRONG -- compiler may reorder */
*(volatile uint32_t *)addr = val;

/* CORRECT -- includes memory barrier */
hal_mmio_write32(addr, val);
```

### 2. Handle All Errors

Check every HAL return value. Never assume success:

```c
hal_status_t ret = hal_irq_register(irq, handler, data);
if (ret != HAL_OK) {
    hal_console_printf("[mydrv] ERROR: IRQ %u registration failed: %d\n", irq, ret);
    return ret;
}
```

### 3. Support All Architectures

Do not use x86-specific features like port I/O unless your device genuinely requires it (e.g., legacy PS/2). For PCI devices, always use MMIO (BAR-based) access, which works on all three architectures.

```c
/* Prefer MMIO */
uint32_t status = hal_mmio_read32(dev->bar[0] + REG_STATUS);

/* Only use port I/O for legacy devices that require it */
#if defined(__x86_64__)
uint8_t val = hal_io_inb(0x60);  /* PS/2 data port */
#endif
```

### 4. Clean Shutdown

Always implement `shutdown()` properly. Free all DMA buffers, unregister all interrupts, and disable the device hardware. This is essential for driver hot-swapping.

### 5. Minimize State

Use a single static state struct per driver instance. Avoid dynamic memory allocation within drivers when possible -- use statically sized buffers or HAL DMA allocation.

### 6. Log Meaningful Messages

Use `hal_console_printf()` with a driver tag prefix for all log messages:

```c
hal_console_printf("[e1000] Link up at 1000 Mbps\n");
hal_console_printf("[e1000] ERROR: TX timeout after 5000 ms\n");
```

### 7. Respect the PCI Configuration Space

Always enable Bus Master and Memory Space before accessing device BARs:

```c
uint32_t cmd = hal_bus_read_config(bus, dev, func, 0x04, 2);
cmd |= (1 << 1) | (1 << 2);  /* Memory Space + Bus Master */
hal_bus_write_config(bus, dev, func, 0x04, 2, cmd);
```

### 8. Use DMA for Performance

For high-throughput devices (NICs, NVMe), always use DMA. Allocate ring buffers with `hal_dma_alloc()` and pass the physical address to the device.

### 9. Test on All Architectures

Run your driver in QEMU for all three architectures before submitting. Many subtle bugs only manifest on one architecture (e.g., endianness assumptions, alignment requirements).

### 10. Document Your Driver

Add a comment block at the top of your source file describing:

- The device(s) supported (vendor/device IDs)
- Key registers and their offsets
- Any hardware quirks or limitations
- References to the device specification

---

## Complete Minimal Driver Example

Here is a complete, compilable skeleton for a network driver:

```c
/*
 * my_nic.c -- AlJefra OS driver for My Custom NIC
 *
 * Vendor ID: 0x1234
 * Device ID: 0x5678
 *
 * Reference: My Custom NIC Programming Manual, Rev. 2.0
 */

#include "hal/hal.h"

/* PCI identification */
#define MY_NIC_VENDOR  0x1234
#define MY_NIC_DEVICE  0x5678

/* Register offsets (MMIO, from BAR0) */
#define REG_CTRL       0x0000
#define REG_STATUS     0x0008
#define REG_RXBASE     0x0100
#define REG_TXBASE     0x0200
#define REG_MAC_LO     0x0040
#define REG_MAC_HI     0x0044
#define REG_ICR        0x00C0

/* Bits */
#define CTRL_RESET     (1 << 0)
#define CTRL_RXEN      (1 << 1)
#define CTRL_TXEN      (1 << 2)
#define STATUS_LINK    (1 << 0)

/* Driver state */
static struct {
    volatile void *base;
    uint8_t mac[6];
    uint8_t irq;
    void *rx_ring;
    void *tx_ring;
    uint64_t rx_phys;
    uint64_t tx_phys;
} nic;

/* Forward declarations */
static hal_status_t my_nic_init(void);
static void         my_nic_shutdown(void);
static int          my_nic_send(const void *pkt, uint32_t len);
static int          my_nic_recv(void *buf, uint32_t max_len);
static void         my_nic_get_mac(uint8_t mac[6]);
static void         my_nic_irq(uint32_t irq, void *data);

/* Exported driver_ops_t */
driver_ops_t my_nic_driver = {
    .name     = "my-nic",
    .category = DRIVER_NETWORK,
    .init     = my_nic_init,
    .shutdown = my_nic_shutdown,
    .ops.network = {
        .send    = my_nic_send,
        .recv    = my_nic_recv,
        .get_mac = my_nic_get_mac,
    },
};

/* --- Implementation --- */

static hal_device_t found_dev;
static int dev_found = 0;

static void scan_cb(hal_device_t *dev) {
    if (dev->vendor_id == MY_NIC_VENDOR && dev->device_id == MY_NIC_DEVICE) {
        found_dev = *dev;
        dev_found = 1;
    }
}

static hal_status_t my_nic_init(void) {
    /* Find device */
    dev_found = 0;
    hal_bus_scan(scan_cb);
    if (!dev_found) {
        return HAL_NO_DEVICE;
    }

    nic.base = (volatile void *)found_dev.bar[0];
    nic.irq  = found_dev.irq;

    /* Enable PCI bus mastering */
    uint32_t cmd = hal_bus_read_config(found_dev.bus, found_dev.dev,
                                       found_dev.func, 0x04, 2);
    cmd |= (1 << 1) | (1 << 2);
    hal_bus_write_config(found_dev.bus, found_dev.dev,
                         found_dev.func, 0x04, 2, cmd);

    /* Reset device */
    hal_mmio_write32(nic.base + REG_CTRL, CTRL_RESET);
    hal_timer_delay_us(10000);  /* Wait 10 ms for reset */

    /* Read MAC address */
    uint32_t mac_lo = hal_mmio_read32(nic.base + REG_MAC_LO);
    uint32_t mac_hi = hal_mmio_read32(nic.base + REG_MAC_HI);
    nic.mac[0] = (mac_lo >>  0) & 0xFF;
    nic.mac[1] = (mac_lo >>  8) & 0xFF;
    nic.mac[2] = (mac_lo >> 16) & 0xFF;
    nic.mac[3] = (mac_lo >> 24) & 0xFF;
    nic.mac[4] = (mac_hi >>  0) & 0xFF;
    nic.mac[5] = (mac_hi >>  8) & 0xFF;

    /* Allocate DMA rings */
    nic.rx_ring = hal_dma_alloc(4096, &nic.rx_phys);
    nic.tx_ring = hal_dma_alloc(4096, &nic.tx_phys);
    if (!nic.rx_ring || !nic.tx_ring) {
        return HAL_NO_MEMORY;
    }

    /* Program RX/TX ring addresses */
    hal_mmio_write64(nic.base + REG_RXBASE, nic.rx_phys);
    hal_mmio_write64(nic.base + REG_TXBASE, nic.tx_phys);

    /* Register IRQ */
    hal_irq_register(nic.irq, my_nic_irq, NULL);
    hal_irq_enable(nic.irq);

    /* Enable RX and TX */
    hal_mmio_write32(nic.base + REG_CTRL, CTRL_RXEN | CTRL_TXEN);

    hal_console_printf("[my-nic] Initialized: MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                       nic.mac[0], nic.mac[1], nic.mac[2],
                       nic.mac[3], nic.mac[4], nic.mac[5]);
    return HAL_OK;
}

static void my_nic_shutdown(void) {
    hal_mmio_write32(nic.base + REG_CTRL, 0);
    hal_irq_disable(nic.irq);
    hal_irq_unregister(nic.irq);
    hal_dma_free(nic.rx_ring, 4096);
    hal_dma_free(nic.tx_ring, 4096);
    hal_console_printf("[my-nic] Shut down.\n");
}

static int my_nic_send(const void *pkt, uint32_t len) {
    /* TODO: Set up TX descriptor and signal hardware */
    (void)pkt;
    (void)len;
    return 0;
}

static int my_nic_recv(void *buf, uint32_t max_len) {
    /* TODO: Check RX descriptor ring for completed receives */
    (void)buf;
    (void)max_len;
    return 0;
}

static void my_nic_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++)
        mac[i] = nic.mac[i];
}

static void my_nic_irq(uint32_t irq, void *data) {
    (void)data;
    uint32_t icr = hal_mmio_read32(nic.base + REG_ICR);
    /* TODO: Handle RX/TX completion based on ICR bits */
    (void)icr;
    hal_irq_eoi(irq);
}
```

---

## Further Reading

- [HAL Specification](hal_spec.md) -- Complete API reference for all HAL functions
- [Architecture](architecture.md) -- System design and layer model
- [Marketplace Spec](marketplace_spec.md) -- How to publish drivers to the AlJefra Store
- [Boot Protocol](boot_protocol.md) -- How drivers are loaded during boot

---

*AlJefra OS v1.0 -- Driver Development Guide -- Built in Qatar by [Qatar IT](https://www.qatarit.com)*
