/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Architecture-Independent Kernel Entry
 *
 * Called after arch-specific boot code has:
 *   1. Set up CPU, MMU, interrupts, timer
 *   2. Initialized early console (UART/serial)
 *   3. Called hal_init()
 *
 * This module orchestrates the rest of the boot:
 *   Bus scan → driver matching → network → AI bootstrap → interactive
 */

#include "../hal/hal.h"
#include "sched.h"
#include "syscall.h"
#include "driver_loader.h"
#include "ai_bootstrap.h"

/* Forward declarations for subsystem init */
static void banner(void);
static void detect_hardware(void);
static void register_builtin_drivers(void);
static void load_builtin_drivers(void);
static void start_network(void);

/* Forward declarations for built-in driver registration */
extern void e1000_register(void);
extern void rtl8169_register(void);
extern void virtio_net_register(void);
extern void virtio_blk_register(void);
extern void ahci_register(void);
extern void nvme_register(void);

/* ── Hardware manifest (filled by bus scan) ── */
static hal_device_t g_devices[HAL_BUS_MAX_DEVICES];
static uint32_t     g_device_count;

/* ── Kernel entry point ── */
void kernel_main(void)
{
    banner();

    /* Phase 0: Register built-in driver ops tables */
    register_builtin_drivers();

    /* Phase 1: Hardware discovery */
    hal_console_puts("[kernel] Scanning buses...\n");
    detect_hardware();

    /* Phase 2: Load built-in drivers (compiled into kernel image) */
    hal_console_puts("[kernel] Loading built-in drivers...\n");
    load_builtin_drivers();

    /* Phase 3: Initialize scheduler */
    hal_console_puts("[kernel] Starting scheduler...\n");
    sched_init();

    /* Phase 4: Bring up network */
    hal_console_puts("[kernel] Starting network...\n");
    start_network();

    /* Phase 5: AI bootstrap — connect to marketplace, download drivers */
    hal_console_puts("[kernel] Starting AI bootstrap...\n");
    ai_bootstrap(g_devices, g_device_count);

    /* Phase 6: Interactive — kernel is fully up */
    hal_console_puts("[kernel] AlJefra OS ready.\n");

    /* Main loop: handle syscalls and interrupts */
    syscall_loop();

    /* Should never reach here */
    hal_console_puts("[kernel] PANIC: syscall_loop returned\n");
    for (;;)
        hal_cpu_halt();
}

/* ── Boot banner ── */
static void banner(void)
{
    hal_console_puts("\n");
    hal_console_puts("==============================================\n");
    hal_console_puts("  AlJefra OS — Universal Boot\n");
    hal_console_puts("  Architecture: ");

    switch (hal_arch()) {
    case HAL_ARCH_X86_64:  hal_console_puts("x86-64\n");  break;
    case HAL_ARCH_AARCH64: hal_console_puts("AArch64\n"); break;
    case HAL_ARCH_RISCV64: hal_console_puts("RISC-V 64\n"); break;
    }

    hal_cpu_info_t cpu;
    hal_cpu_get_info(&cpu);
    hal_console_puts("  CPU: ");
    hal_console_puts(cpu.model);
    hal_console_puts("\n");
    hal_console_printf("  Cores: %u\n", cpu.cores_logical);
    hal_console_printf("  RAM: %u MB\n", (uint32_t)(hal_mmu_total_ram() / (1024 * 1024)));
    hal_console_puts("==============================================\n\n");
}

/* ── Hardware discovery ── */
static void detect_hardware(void)
{
    g_device_count = hal_bus_scan(g_devices, HAL_BUS_MAX_DEVICES);
    hal_console_printf("[kernel] Found %u devices\n", g_device_count);

    for (uint32_t i = 0; i < g_device_count; i++) {
        hal_device_t *d = &g_devices[i];
        if (d->bus_type == HAL_BUS_PCIE) {
            hal_console_printf("  [%02x:%02x.%x] %04x:%04x class %02x:%02x\n",
                               d->bus, d->dev, d->func,
                               d->vendor_id, d->device_id,
                               d->class_code, d->subclass);
        }
    }
}

/* ── PCI class codes for driver matching ── */
#define PCI_CLASS_STORAGE      0x01
#define PCI_CLASS_NETWORK      0x02
#define PCI_CLASS_DISPLAY      0x03
#define PCI_CLASS_SERIAL_BUS   0x0C

#define PCI_SUBCLASS_NVME      0x08
#define PCI_SUBCLASS_AHCI      0x06
#define PCI_SUBCLASS_ETHERNET  0x00
#define PCI_SUBCLASS_USB       0x03

/* ── Register all built-in driver ops tables ── */
static void register_builtin_drivers(void)
{
    e1000_register();
    rtl8169_register();
    virtio_net_register();
    virtio_blk_register();
    ahci_register();
    nvme_register();
}

/* ── Load built-in (compiled-in) drivers ── */
static void load_builtin_drivers(void)
{
    uint32_t loaded = 0;

    for (uint32_t i = 0; i < g_device_count; i++) {
        hal_device_t *d = &g_devices[i];
        hal_status_t rc;

        /* Storage: NVMe */
        if (d->class_code == PCI_CLASS_STORAGE && d->subclass == PCI_SUBCLASS_NVME) {
            rc = driver_load_builtin("nvme", d);
            if (rc == HAL_OK) loaded++;
        }

        /* Storage: AHCI/SATA */
        if (d->class_code == PCI_CLASS_STORAGE && d->subclass == PCI_SUBCLASS_AHCI) {
            rc = driver_load_builtin("ahci", d);
            if (rc == HAL_OK) loaded++;
        }

        /* Network: Intel e1000/e1000e */
        if (d->class_code == PCI_CLASS_NETWORK && d->subclass == PCI_SUBCLASS_ETHERNET) {
            if (d->vendor_id == 0x8086) {
                rc = driver_load_builtin("e1000", d);
                if (rc == HAL_OK) loaded++;
            }
        }

        /* Network: Realtek RTL8169/RTL8168/RTL8111/RTL8101E */
        if (d->class_code == PCI_CLASS_NETWORK && d->subclass == PCI_SUBCLASS_ETHERNET) {
            if (d->vendor_id == 0x10EC &&
                (d->device_id == 0x8136 || d->device_id == 0x8161 ||
                 d->device_id == 0x8168 || d->device_id == 0x8169)) {
                rc = driver_load_builtin("rtl8169", d);
                if (rc == HAL_OK) loaded++;
            }
        }

        /* Network: VirtIO-Net */
        if (d->vendor_id == 0x1AF4 && (d->device_id >= 0x1000 && d->device_id <= 0x103F)) {
            if (d->subclass == 0x00) {
                rc = driver_load_builtin("virtio-net", d);
                if (rc == HAL_OK) loaded++;
            }
        }

        /* Storage: VirtIO-Blk */
        if (d->vendor_id == 0x1AF4 && d->device_id == 0x1001) {
            rc = driver_load_builtin("virtio-blk", d);
            if (rc == HAL_OK) loaded++;
        }

        /* USB: xHCI */
        if (d->class_code == PCI_CLASS_SERIAL_BUS && d->subclass == PCI_SUBCLASS_USB) {
            if (d->prog_if == 0x30) { /* xHCI */
                rc = driver_load_builtin("xhci", d);
                if (rc == HAL_OK) loaded++;
            }
        }
    }

    hal_console_printf("[kernel] Loaded %u built-in drivers\n", loaded);
}

/* ── Network startup ── */
static void start_network(void)
{
    /* The AI bootstrap module handles DHCP + full network bringup */
    hal_console_puts("[kernel] Network will be configured by AI bootstrap\n");
}
