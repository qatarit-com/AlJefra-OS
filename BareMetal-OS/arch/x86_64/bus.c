/* SPDX-License-Identifier: MIT */
/* AlJefra OS — x86-64 Bus HAL Implementation
 * PCIe device enumeration and config space access via b_system(BUS_READ/BUS_WRITE).
 *
 * BareMetal kernel convention for BUS_READ / BUS_WRITE:
 *   RAX = BDF << 8 | register_offset   (bus:dev:func packed into upper bits)
 *   Specifically: RAX = (bus << 16) | (dev << 11) | (func << 8) | (reg & 0xFC)
 *   RDX = value (for writes)
 *   Returns: RAX = value read (for reads)
 */

#include "../../hal/hal.h"

/* BareMetal kernel API */
extern uint64_t b_system(uint64_t function, uint64_t var1, uint64_t var2);

/* b_system function codes */
#define SYS_BUS_READ   0x50
#define SYS_BUS_WRITE  0x51

/* PCI configuration register offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_CLASS_CODE      0x08  /* Byte 3 = class, byte 2 = subclass, byte 1 = prog IF */
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C

/* PCI command register bits */
#define PCI_CMD_IO_SPACE      (1u << 0)
#define PCI_CMD_MEMORY_SPACE  (1u << 1)
#define PCI_CMD_BUS_MASTER    (1u << 2)

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/* Pack bus/dev/func/reg into the RDX format expected by BareMetal os_bus_read/write:
 *   Bits 31:24 = Bus
 *   Bits 23:16 = Device/Function (dev<<3 | func)
 *   Bits 15:0  = Register INDEX (byte_offset / 4)
 * Note: reg parameter is in byte offset (e.g. 0x00, 0x08, 0x10) */
static inline uint64_t pci_addr(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg)
{
    uint32_t devfunc = (dev << 3) | func;
    uint32_t reg_index = reg >> 2;
    return ((uint64_t)bus << 24) | ((uint64_t)devfunc << 16) | reg_index;
}

/* Pack BDF from a combined uint32_t: bdf = (bus<<8 | dev<<3 | func) */
static inline uint64_t bdf_to_addr(uint32_t bdf, uint32_t reg)
{
    uint32_t bus  = (bdf >> 8) & 0xFF;
    uint32_t devfunc = bdf & 0xFF;
    return ((uint64_t)bus << 24) | ((uint64_t)devfunc << 16) | (reg >> 2);
}

/* Read 32-bit PCI config space via kernel
 * os_bus_read expects: RDX = packed bus address, returns EAX = value
 * b_system ABI: RAX = var1, RDX = var2
 * So address must go in var2 (RDX). */
static inline uint32_t pci_read32(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg)
{
    uint64_t addr = pci_addr(bus, dev, func, reg);
    return (uint32_t)b_system(SYS_BUS_READ, 0, addr);
}

/* Write 32-bit PCI config space via kernel
 * os_bus_write expects: RDX = packed bus address, EAX = value to write
 * b_system ABI: RAX = var1, RDX = var2
 * So value goes in var1 (RAX), address in var2 (RDX). */
static inline void pci_write32(uint32_t bus, uint32_t dev, uint32_t func, uint32_t reg, uint32_t val)
{
    uint64_t addr = pci_addr(bus, dev, func, reg);
    b_system(SYS_BUS_WRITE, (uint64_t)val, addr);
}

/* Probe a single BAR and return its physical base address and size */
static void probe_bar(uint32_t bus, uint32_t dev, uint32_t func,
                       uint32_t bar_reg, uint64_t *base_out, uint64_t *size_out)
{
    /* Read original BAR value */
    uint32_t orig = pci_read32(bus, dev, func, bar_reg);

    /* Determine BAR type */
    if (orig & 1) {
        /* I/O BAR */
        *base_out = orig & ~0x03u;

        /* Write all 1s to determine size */
        pci_write32(bus, dev, func, bar_reg, 0xFFFFFFFF);
        uint32_t mask = pci_read32(bus, dev, func, bar_reg);
        pci_write32(bus, dev, func, bar_reg, orig); /* Restore */

        mask &= ~0x03u;
        if (mask == 0) {
            *size_out = 0;
        } else {
            *size_out = (~mask) + 1;
        }
    } else {
        /* Memory BAR */
        uint32_t type = (orig >> 1) & 0x03;

        if (type == 0x02) {
            /* 64-bit BAR: spans two consecutive registers */
            uint32_t orig_hi = pci_read32(bus, dev, func, bar_reg + 4);
            *base_out = ((uint64_t)orig_hi << 32) | (orig & ~0x0Fu);

            /* Probe size: write all 1s to both registers */
            pci_write32(bus, dev, func, bar_reg, 0xFFFFFFFF);
            pci_write32(bus, dev, func, bar_reg + 4, 0xFFFFFFFF);
            uint32_t mask_lo = pci_read32(bus, dev, func, bar_reg);
            uint32_t mask_hi = pci_read32(bus, dev, func, bar_reg + 4);
            pci_write32(bus, dev, func, bar_reg, orig);
            pci_write32(bus, dev, func, bar_reg + 4, orig_hi);

            uint64_t mask64 = ((uint64_t)mask_hi << 32) | (mask_lo & ~0x0FUL);
            if (mask64 == 0) {
                *size_out = 0;
            } else {
                *size_out = (~mask64) + 1;
            }
        } else {
            /* 32-bit BAR */
            *base_out = orig & ~0x0Fu;

            pci_write32(bus, dev, func, bar_reg, 0xFFFFFFFF);
            uint32_t mask = pci_read32(bus, dev, func, bar_reg);
            pci_write32(bus, dev, func, bar_reg, orig);

            mask &= ~0x0Fu;
            if (mask == 0) {
                *size_out = 0;
            } else {
                *size_out = (~mask) + 1;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* HAL Bus API                                                                */
/* -------------------------------------------------------------------------- */

hal_status_t hal_bus_init(void)
{
    /* The BareMetal kernel initializes PCIe config space access during boot.
     * Nothing additional needed here. */
    return HAL_OK;
}

uint32_t hal_bus_scan(hal_device_t *devs, uint32_t max)
{
    uint32_t count = 0;

    /* Enumerate bus 0-255, device 0-31, function 0-7 */
    for (uint32_t bus = 0; bus < 256 && count < max; bus++) {
        for (uint32_t dev = 0; dev < 32 && count < max; dev++) {
            for (uint32_t func = 0; func < 8 && count < max; func++) {
                uint32_t id = pci_read32(bus, dev, func, PCI_VENDOR_ID);
                uint16_t vendor = id & 0xFFFF;
                uint16_t device = (id >> 16) & 0xFFFF;

                /* No device present */
                if (vendor == 0xFFFF || vendor == 0x0000)
                    continue;

                hal_device_t *d = &devs[count];

                /* Zero the structure */
                for (uint64_t i = 0; i < sizeof(hal_device_t); i++)
                    ((uint8_t *)d)[i] = 0;

                d->bus_type  = HAL_BUS_PCIE;
                d->vendor_id = vendor;
                d->device_id = device;
                d->bus       = (uint16_t)bus;
                d->dev       = (uint8_t)dev;
                d->func      = (uint8_t)func;

                /* Class code register (offset 0x08): rev|progIF|subclass|class */
                uint32_t class_reg = pci_read32(bus, dev, func, PCI_CLASS_CODE);
                d->class_code = (class_reg >> 24) & 0xFF;
                d->subclass   = (class_reg >> 16) & 0xFF;
                d->prog_if    = (class_reg >> 8)  & 0xFF;

                /* Interrupt line */
                uint32_t int_reg = pci_read32(bus, dev, func, PCI_INTERRUPT_LINE);
                d->irq = (uint8_t)(int_reg & 0xFF);

                /* Probe BARs */
                uint32_t header = pci_read32(bus, dev, func, PCI_CACHE_LINE_SIZE);
                uint8_t header_type = (header >> 16) & 0x7F;

                int max_bars = (header_type == 0) ? 6 : 2;
                for (int b = 0; b < max_bars && b < HAL_BUS_MAX_BARS; b++) {
                    uint32_t bar_reg = PCI_BAR0 + (b * 4);
                    probe_bar(bus, dev, func, bar_reg,
                              &d->bar[b], &d->bar_size[b]);

                    /* If this is a 64-bit BAR, skip the next index */
                    uint32_t bar_val = pci_read32(bus, dev, func, bar_reg);
                    if (!(bar_val & 1) && (((bar_val >> 1) & 0x03) == 0x02)) {
                        b++; /* Skip upper 32 bits of 64-bit BAR */
                    }
                }

                count++;

                /* If not multi-function, only check function 0 */
                if (func == 0) {
                    uint32_t ht = pci_read32(bus, dev, func, PCI_CACHE_LINE_SIZE);
                    if (!((ht >> 16) & 0x80))
                        break; /* Not multi-function */
                }
            }
        }
    }

    return count;
}

uint32_t hal_bus_pci_read32(uint32_t bdf, uint32_t reg)
{
    uint64_t addr = bdf_to_addr(bdf, reg);
    return (uint32_t)b_system(SYS_BUS_READ, 0, addr);
}

void hal_bus_pci_write32(uint32_t bdf, uint32_t reg, uint32_t val)
{
    uint64_t addr = bdf_to_addr(bdf, reg);
    b_system(SYS_BUS_WRITE, (uint64_t)val, addr);
}

void hal_bus_pci_enable(hal_device_t *dev)
{
    uint32_t bdf_addr = pci_addr(dev->bus, dev->dev, dev->func, PCI_COMMAND);

    uint32_t cmd = (uint32_t)b_system(SYS_BUS_READ, 0, bdf_addr);
    cmd |= PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER;
    b_system(SYS_BUS_WRITE, (uint64_t)cmd, bdf_addr);
}

volatile void *hal_bus_map_bar(hal_device_t *dev, uint32_t bar_index)
{
    if (bar_index >= HAL_BUS_MAX_BARS)
        return (volatile void *)0;

    /* Identity-mapped on bare-metal: physical address == virtual address */
    uint64_t phys = dev->bar[bar_index];
    if (phys == 0)
        return (volatile void *)0;

    return (volatile void *)(uintptr_t)phys;
}

uint32_t hal_bus_find_by_class(uint8_t class_code, uint8_t subclass,
                                hal_device_t *out, uint32_t max)
{
    /* Scan all devices, filter by class */
    hal_device_t all_devs[HAL_BUS_MAX_DEVICES];
    uint32_t total = hal_bus_scan(all_devs, HAL_BUS_MAX_DEVICES);
    uint32_t found = 0;

    for (uint32_t i = 0; i < total && found < max; i++) {
        if (all_devs[i].class_code == class_code &&
            all_devs[i].subclass == subclass) {
            /* Copy device info */
            uint8_t *dst = (uint8_t *)&out[found];
            uint8_t *src = (uint8_t *)&all_devs[i];
            for (uint64_t j = 0; j < sizeof(hal_device_t); j++)
                dst[j] = src[j];
            found++;
        }
    }

    return found;
}

uint32_t hal_bus_find_by_id(uint16_t vendor, uint16_t device,
                             hal_device_t *out, uint32_t max)
{
    /* Scan all devices, filter by vendor/device ID */
    hal_device_t all_devs[HAL_BUS_MAX_DEVICES];
    uint32_t total = hal_bus_scan(all_devs, HAL_BUS_MAX_DEVICES);
    uint32_t found = 0;

    for (uint32_t i = 0; i < total && found < max; i++) {
        if (all_devs[i].vendor_id == vendor &&
            all_devs[i].device_id == device) {
            uint8_t *dst = (uint8_t *)&out[found];
            uint8_t *src = (uint8_t *)&all_devs[i];
            for (uint64_t j = 0; j < sizeof(hal_device_t); j++)
                dst[j] = src[j];
            found++;
        }
    }

    return found;
}
