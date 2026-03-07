# AlJefra OS -- HAL Interface Specification

## Overview

The Hardware Abstraction Layer (HAL) provides architecture-independent C interfaces for all hardware operations. Code above the HAL -- drivers, kernel, network stack, AI agent -- never uses inline assembly or architecture-specific features. Each supported architecture (x86-64, ARM64, RISC-V 64) provides its own implementation of these interfaces in the `arch/` directory.

The HAL is defined across **9 header files** in the `hal/` directory:

| Header | Purpose |
|--------|---------|
| `hal.h` | Master header -- includes all others, defines common types |
| `cpu.h` | CPU initialization, identification, frequency, halt |
| `interrupt.h` | IRQ registration, enable, disable, end-of-interrupt |
| `timer.h` | Tick counter, delays, one-shot callbacks |
| `bus.h` | Bus enumeration and configuration space access |
| `io.h` | MMIO read/write, port I/O, DMA allocation |
| `mmu.h` | Virtual memory mapping, page allocation |
| `smp.h` | Multi-core management and spinlocks |
| `console.h` | Early text output (serial, VGA, UART) |

---

## Common Types

Defined in `hal/hal.h`:

### hal_status_t

Return type for all HAL functions that can fail.

```c
typedef enum {
    HAL_OK            = 0,   /* Operation succeeded                    */
    HAL_ERROR         = -1,  /* Generic error                          */
    HAL_TIMEOUT       = -2,  /* Operation timed out                    */
    HAL_BUSY          = -3,  /* Resource is busy, try again            */
    HAL_NO_DEVICE     = -4,  /* Device not found                       */
    HAL_NO_MEMORY     = -5,  /* Out of memory                          */
    HAL_NOT_SUPPORTED = -6   /* Operation not supported on this arch   */
} hal_status_t;
```

### hal_arch_t

Identifies the current architecture at runtime.

```c
typedef enum {
    HAL_ARCH_X86_64   = 1,
    HAL_ARCH_AARCH64  = 2,
    HAL_ARCH_RISCV64  = 3
} hal_arch_t;
```

Use `hal_arch()` to query the current architecture:

```c
hal_arch_t hal_arch(void);
```

### hal_device_t

Describes a device discovered during bus enumeration.

```c
typedef struct {
    uint16_t    bus_type;       /* 0 = PCI, 1 = USB, 2 = Platform          */
    uint16_t    vendor_id;      /* PCI/USB vendor ID                       */
    uint16_t    device_id;      /* PCI/USB device/product ID               */
    uint16_t    class_code;     /* PCI class code                          */
    uint16_t    subclass;       /* PCI subclass                            */
    uint8_t     prog_if;        /* PCI programming interface               */
    uint8_t     irq;            /* Assigned IRQ number                     */
    uint64_t    bar[6];         /* Base Address Registers (PCI BARs)       */
    uint64_t    bar_size[6];    /* Size of each BAR region                 */
    uint8_t     bus;            /* PCI bus number                          */
    uint8_t     dev;            /* PCI device number                       */
    uint8_t     func;           /* PCI function number                     */
    char        compatible[64]; /* Compatible string (DTB, platform devs)  */
} hal_device_t;
```

### Interrupt Handler Type

```c
typedef void (*hal_irq_handler_t)(uint32_t irq, void *data);
```

### Timer Callback Type

```c
typedef void (*hal_timer_callback_t)(void *data);
```

---

## CPU API (`hal/cpu.h`)

Provides CPU initialization, identification, and control.

### Functions

```c
/*
 * hal_cpu_init -- Initialize the BSP (bootstrap processor).
 *
 * Called once during hal_init(). Sets up CPU-specific features
 * (e.g., CPUID on x86, system registers on ARM, CSRs on RISC-V).
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_cpu_init(void);

/*
 * hal_cpu_id -- Return the current CPU/core/hart ID.
 *
 * On x86-64: APIC ID.
 * On ARM64:  MPIDR_EL1 affinity.
 * On RISC-V: Hart ID.
 */
uint32_t hal_cpu_id(void);

/*
 * hal_cpu_freq -- Return the CPU frequency in Hz.
 *
 * May return an estimate based on calibration (e.g., TSC on x86,
 * CNTFRQ_EL0 on ARM, timebase on RISC-V).
 */
uint64_t hal_cpu_freq(void);

/*
 * hal_cpu_halt -- Halt the current CPU until the next interrupt.
 *
 * On x86-64: HLT instruction.
 * On ARM64:  WFI instruction.
 * On RISC-V: WFI instruction.
 */
void hal_cpu_halt(void);

/*
 * hal_cpu_enable_interrupts -- Enable interrupts on the current CPU.
 *
 * On x86-64: STI.
 * On ARM64:  Clear DAIF.I.
 * On RISC-V: Set MIE/SIE in mstatus/sstatus.
 */
void hal_cpu_enable_interrupts(void);

/*
 * hal_cpu_disable_interrupts -- Disable interrupts on the current CPU.
 *
 * On x86-64: CLI.
 * On ARM64:  Set DAIF.I.
 * On RISC-V: Clear MIE/SIE in mstatus/sstatus.
 */
void hal_cpu_disable_interrupts(void);

/*
 * Memory barriers -- Ensure memory ordering across CPUs.
 */
void hal_cpu_memory_barrier(void);      /* Full barrier (mfence / DSB / fence) */
void hal_cpu_read_barrier(void);        /* Read barrier  (lfence / DSB LD / fence r,r) */
void hal_cpu_write_barrier(void);       /* Write barrier (sfence / DSB ST / fence w,w) */
```

---

## Interrupt API (`hal/interrupt.h`)

Provides IRQ management for all architectures. The HAL maps hardware-specific interrupt controllers (APIC, GIC, PLIC) to a uniform numbered IRQ interface.

### Functions

```c
/*
 * hal_irq_init -- Initialize the interrupt controller.
 *
 * On x86-64: Set up IDT, configure I/O APIC.
 * On ARM64:  Initialize GICv2 or GICv3 distributor + CPU interface.
 * On RISC-V: Configure PLIC thresholds and priorities.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_irq_init(void);

/*
 * hal_irq_register -- Register a handler for a specific IRQ.
 *
 * @irq:     IRQ number (architecture-normalized).
 * @handler: Function to call when the IRQ fires.
 * @data:    Opaque pointer passed to the handler.
 *
 * Returns: HAL_OK on success, HAL_ERROR if the IRQ is already registered.
 */
hal_status_t hal_irq_register(uint32_t irq, hal_irq_handler_t handler, void *data);

/*
 * hal_irq_unregister -- Remove the handler for a specific IRQ.
 *
 * @irq: IRQ number to unregister.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_irq_unregister(uint32_t irq);

/*
 * hal_irq_enable -- Enable (unmask) a specific IRQ.
 *
 * @irq: IRQ number to enable.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_irq_enable(uint32_t irq);

/*
 * hal_irq_disable -- Disable (mask) a specific IRQ.
 *
 * @irq: IRQ number to disable.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_irq_disable(uint32_t irq);

/*
 * hal_irq_eoi -- Signal end-of-interrupt to the controller.
 *
 * Must be called at the end of every interrupt handler.
 *
 * @irq: IRQ number that was serviced.
 */
void hal_irq_eoi(uint32_t irq);
```

### Usage Example

```c
static void my_irq_handler(uint32_t irq, void *data) {
    /* Handle the interrupt */
    /* ... */
    hal_irq_eoi(irq);
}

void setup_device(void) {
    hal_irq_register(11, my_irq_handler, NULL);
    hal_irq_enable(11);
}
```

---

## Timer API (`hal/timer.h`)

Provides a monotonic tick counter, microsecond delays, and one-shot timer callbacks.

### Functions

```c
/*
 * hal_timer_init -- Initialize the system timer.
 *
 * @freq_hz: Desired tick frequency in Hz (e.g., 1000 for 1 ms ticks).
 *
 * On x86-64: Programs HPET (or PIT 8254 as fallback).
 * On ARM64:  Configures the Generic Timer.
 * On RISC-V: Programs CLINT mtimecmp.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_timer_init(uint32_t freq_hz);

/*
 * hal_timer_ticks -- Return the number of ticks since timer init.
 *
 * Monotonically increasing. Resolution depends on freq_hz.
 */
uint64_t hal_timer_ticks(void);

/*
 * hal_timer_freq -- Return the actual tick frequency in Hz.
 *
 * May differ slightly from the requested frequency due to
 * hardware limitations.
 */
uint32_t hal_timer_freq(void);

/*
 * hal_timer_delay_us -- Busy-wait for the specified number of microseconds.
 *
 * @us: Microseconds to delay. Guaranteed to wait at least this long.
 */
void hal_timer_delay_us(uint64_t us);

/*
 * hal_timer_oneshot -- Schedule a one-shot callback after a delay.
 *
 * @us:       Delay in microseconds.
 * @callback: Function to call when the timer fires.
 * @data:     Opaque pointer passed to the callback.
 *
 * Returns: HAL_OK on success, HAL_ERROR if no timer channel is available.
 */
hal_status_t hal_timer_oneshot(uint64_t us, hal_timer_callback_t callback, void *data);
```

### Usage Example

```c
/* Wait 100 milliseconds */
hal_timer_delay_us(100 * 1000);

/* Get uptime in milliseconds */
uint64_t uptime_ms = hal_timer_ticks() * 1000 / hal_timer_freq();
```

---

## Bus API (`hal/bus.h`)

Provides PCI (and platform) bus enumeration and configuration space access.

### Functions

```c
/*
 * hal_bus_scan -- Enumerate all devices on the bus.
 *
 * Calls the provided callback once for each device found.
 * On x86-64 and RISC-V: scans PCI configuration space.
 * On ARM64: scans PCI ECAM and platform devices from DTB.
 *
 * @callback: Function called for each discovered device.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_bus_scan(void (*callback)(hal_device_t *dev));

/*
 * hal_bus_read_config -- Read a value from PCI configuration space.
 *
 * @bus:    PCI bus number.
 * @dev:    PCI device number.
 * @func:   PCI function number.
 * @offset: Byte offset into configuration space.
 * @size:   Read size in bytes (1, 2, or 4).
 *
 * Returns: The value read.
 */
uint32_t hal_bus_read_config(uint8_t bus, uint8_t dev, uint8_t func,
                             uint16_t offset, uint8_t size);

/*
 * hal_bus_write_config -- Write a value to PCI configuration space.
 *
 * @bus:    PCI bus number.
 * @dev:    PCI device number.
 * @func:   PCI function number.
 * @offset: Byte offset into configuration space.
 * @size:   Write size in bytes (1, 2, or 4).
 * @value:  Value to write.
 */
void hal_bus_write_config(uint8_t bus, uint8_t dev, uint8_t func,
                          uint16_t offset, uint8_t size, uint32_t value);
```

### Usage Example

```c
static void on_device_found(hal_device_t *dev) {
    if (dev->vendor_id == 0x8086 && dev->device_id == 0x100E) {
        /* Found Intel e1000 NIC */
        e1000_init(dev);
    }
}

void discover_devices(void) {
    hal_bus_scan(on_device_found);
}
```

---

## I/O API (`hal/io.h`)

Provides memory-mapped I/O (MMIO), port I/O (x86-64 only), and DMA buffer allocation.

### MMIO Functions

```c
/*
 * MMIO read/write -- Access device registers via memory-mapped I/O.
 *
 * These functions include appropriate memory barriers to ensure
 * device register accesses are not reordered by the compiler or CPU.
 *
 * @addr: Virtual address of the MMIO register.
 */
uint8_t  hal_mmio_read8(volatile void *addr);
uint16_t hal_mmio_read16(volatile void *addr);
uint32_t hal_mmio_read32(volatile void *addr);
uint64_t hal_mmio_read64(volatile void *addr);

void hal_mmio_write8(volatile void *addr, uint8_t val);
void hal_mmio_write16(volatile void *addr, uint16_t val);
void hal_mmio_write32(volatile void *addr, uint32_t val);
void hal_mmio_write64(volatile void *addr, uint64_t val);
```

### Port I/O Functions (x86-64 Only)

On ARM64 and RISC-V these return `HAL_NOT_SUPPORTED` or zero. Drivers targeting all architectures should use MMIO instead.

```c
/*
 * Port I/O -- x86-64 IN/OUT instructions.
 *
 * @port: I/O port number (0x0000 - 0xFFFF).
 */
uint8_t  hal_io_inb(uint16_t port);
uint16_t hal_io_inw(uint16_t port);
uint32_t hal_io_inl(uint16_t port);

void hal_io_outb(uint16_t port, uint8_t val);
void hal_io_outw(uint16_t port, uint16_t val);
void hal_io_outl(uint16_t port, uint32_t val);
```

### DMA Functions

```c
/*
 * hal_dma_alloc -- Allocate a physically contiguous, DMA-capable buffer.
 *
 * @size:      Number of bytes to allocate.
 * @phys_addr: [out] Physical address of the allocated buffer.
 *
 * Returns: Virtual address of the buffer, or NULL on failure.
 *
 * The buffer is guaranteed to be:
 *   - Physically contiguous
 *   - Aligned to at least 4096 bytes (page-aligned)
 *   - Below the 4 GB boundary (for 32-bit DMA devices)
 */
void *hal_dma_alloc(size_t size, uint64_t *phys_addr);

/*
 * hal_dma_free -- Free a previously allocated DMA buffer.
 *
 * @vaddr: Virtual address returned by hal_dma_alloc().
 * @size:  Size that was passed to hal_dma_alloc().
 */
void hal_dma_free(void *vaddr, size_t size);
```

### Usage Example

```c
/* Read a device register */
uint32_t status = hal_mmio_read32(dev->bar[0] + REG_STATUS);

/* Allocate a DMA ring buffer */
uint64_t phys;
void *ring = hal_dma_alloc(4096, &phys);
hal_mmio_write64(dev->bar[0] + REG_RING_BASE, phys);
```

---

## MMU API (`hal/mmu.h`)

Provides virtual memory management, including page table operations and physical page allocation.

### Functions

```c
/*
 * hal_mmu_init -- Initialize the MMU and set up kernel page tables.
 *
 * On x86-64: Creates PML4 with identity-mapped lower memory and
 *            higher-half kernel mapping.
 * On ARM64:  Creates Sv48 page tables with TTBR0/TTBR1 split.
 * On RISC-V: Creates Sv39 page tables in satp.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_mmu_init(void);

/*
 * hal_mmu_map -- Map a virtual address range to a physical address range.
 *
 * @vaddr: Virtual address (must be page-aligned).
 * @paddr: Physical address (must be page-aligned).
 * @size:  Number of bytes to map (must be page-aligned).
 * @flags: Mapping flags:
 *           HAL_MMU_READ    (0x01) -- Readable
 *           HAL_MMU_WRITE   (0x02) -- Writable
 *           HAL_MMU_EXEC    (0x04) -- Executable
 *           HAL_MMU_USER    (0x08) -- User-mode accessible
 *           HAL_MMU_NOCACHE (0x10) -- Disable caching (for MMIO)
 *
 * Returns: HAL_OK on success, HAL_NO_MEMORY if page table pages exhausted.
 */
hal_status_t hal_mmu_map(uint64_t vaddr, uint64_t paddr, size_t size, uint32_t flags);

/* MMU mapping flags */
#define HAL_MMU_READ    0x01
#define HAL_MMU_WRITE   0x02
#define HAL_MMU_EXEC    0x04
#define HAL_MMU_USER    0x08
#define HAL_MMU_NOCACHE 0x10

/*
 * hal_mmu_unmap -- Remove a virtual address mapping.
 *
 * @vaddr: Virtual address to unmap (must be page-aligned).
 * @size:  Number of bytes to unmap (must be page-aligned).
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_mmu_unmap(uint64_t vaddr, size_t size);

/*
 * hal_mmu_alloc_page -- Allocate a single physical page (4096 bytes).
 *
 * Returns: Physical address of the allocated page, or 0 on failure.
 */
uint64_t hal_mmu_alloc_page(void);

/*
 * hal_mmu_free_page -- Free a previously allocated physical page.
 *
 * @paddr: Physical address of the page to free.
 */
void hal_mmu_free_page(uint64_t paddr);
```

### Usage Example

```c
/* Map a device BAR into kernel virtual address space */
hal_mmu_map(0xFFFF800040000000ULL, dev->bar[0], dev->bar_size[0],
            HAL_MMU_READ | HAL_MMU_WRITE | HAL_MMU_NOCACHE);
```

---

## SMP API (`hal/smp.h`)

Provides multi-core management and synchronization primitives.

### Functions

```c
/*
 * hal_smp_init -- Initialize SMP support and bring up secondary CPUs.
 *
 * On x86-64: Sends INIT + SIPI to APs detected via ACPI MADT.
 * On ARM64:  Uses PSCI CPU_ON to start secondary cores.
 * On RISC-V: Uses SBI HSM to start secondary harts.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_smp_init(void);

/*
 * hal_smp_cpu_count -- Return the total number of CPUs/cores/harts.
 *
 * Includes the bootstrap processor.
 */
uint32_t hal_smp_cpu_count(void);

/*
 * hal_smp_current_cpu -- Return the ID of the currently executing CPU.
 *
 * Equivalent to hal_cpu_id() but named for clarity in SMP contexts.
 */
uint32_t hal_smp_current_cpu(void);
```

### Spinlock Functions

```c
typedef struct {
    volatile uint32_t lock;
} hal_spinlock_t;

/*
 * hal_spinlock_init -- Initialize a spinlock to the unlocked state.
 */
void hal_spinlock_init(hal_spinlock_t *lock);

/*
 * hal_spinlock_lock -- Acquire a spinlock, spinning until available.
 *
 * Disables interrupts on the current CPU before acquiring.
 */
void hal_spinlock_lock(hal_spinlock_t *lock);

/*
 * hal_spinlock_unlock -- Release a previously acquired spinlock.
 *
 * Re-enables interrupts on the current CPU.
 */
void hal_spinlock_unlock(hal_spinlock_t *lock);

/*
 * hal_spinlock_trylock -- Attempt to acquire a spinlock without spinning.
 *
 * Returns: 1 if the lock was acquired, 0 if it was already held.
 */
int hal_spinlock_trylock(hal_spinlock_t *lock);
```

### Usage Example

```c
static hal_spinlock_t my_lock;

void init(void) {
    hal_spinlock_init(&my_lock);
}

void critical_section(void) {
    hal_spinlock_lock(&my_lock);
    /* Protected access */
    hal_spinlock_unlock(&my_lock);
}
```

---

## Console API (`hal/console.h`)

Provides early text output before the full display driver is loaded. Used for boot messages, kernel panics, and debug output.

### Functions

```c
/*
 * hal_console_init -- Initialize the early console.
 *
 * On x86-64: VGA text mode (0xB8000) + COM1 serial (0x3F8).
 * On ARM64:  PL011 UART at base address from DTB.
 * On RISC-V: SBI console calls or 16550 UART.
 *
 * Returns: HAL_OK on success.
 */
hal_status_t hal_console_init(void);

/*
 * hal_console_putc -- Output a single character.
 *
 * @c: Character to output. '\n' produces a newline (CR+LF on serial).
 */
void hal_console_putc(char c);

/*
 * hal_console_puts -- Output a null-terminated string.
 *
 * @s: String to output.
 */
void hal_console_puts(const char *s);

/*
 * hal_console_printf -- Formatted output (subset of standard printf).
 *
 * Supports: %d, %u, %x, %X, %p, %s, %c, %%, %ld, %lu, %lx.
 * Does NOT support: floating-point (%f, %e, %g), width/precision modifiers.
 *
 * @fmt: Format string.
 * @...: Variable arguments.
 */
void hal_console_printf(const char *fmt, ...);
```

### Usage Example

```c
hal_console_printf("AlJefra OS v0.7.3 booting on %s\n",
                   hal_arch() == HAL_ARCH_X86_64  ? "x86-64"  :
                   hal_arch() == HAL_ARCH_AARCH64  ? "ARM64"   :
                   hal_arch() == HAL_ARCH_RISCV64  ? "RISC-V"  : "unknown");
```

---

## Initialization Order

The HAL subsystems must be initialized in the following order, as later subsystems depend on earlier ones:

```
1. hal_console_init()     -- Needed for debug output during init
2. hal_cpu_init()         -- CPU features needed by MMU and IRQ
3. hal_mmu_init()         -- Paging needed before MMIO mapping
4. hal_irq_init()         -- Interrupt controller needed by timer
5. hal_timer_init(1000)   -- Timer needed by scheduler
6. hal_bus_scan(callback) -- Discover devices (needs MMIO, IRQ)
7. hal_smp_init()         -- Bring up secondary CPUs last
```

This order is enforced by `hal_init()` in each architecture's `hal_init.c`.

---

## Architecture-Specific Notes

### x86-64

- Port I/O (`hal_io_inb/outb/...`) is fully functional. Some legacy devices (PIT, PS/2, COM ports) require port I/O.
- The IDT is set up with 256 entries. IRQ numbers 0-15 map to legacy ISA interrupts; higher numbers map to I/O APIC inputs.
- HPET is preferred over PIT for higher resolution. If HPET is not available, the PIT is used as a fallback.

### ARM64

- Port I/O functions return zero / do nothing. All device access uses MMIO.
- GICv3 is preferred over GICv2 when available.
- DeviceTree (DTB) is used to discover platform devices and their MMIO base addresses.
- The Generic Timer frequency is read from `CNTFRQ_EL0`.

### RISC-V 64

- Port I/O functions return zero / do nothing. All device access uses MMIO.
- PLIC interrupts are edge-triggered by default; the HAL configures priorities to ensure all enabled interrupts fire.
- The CLINT provides per-hart timer interrupts via `mtime` and `mtimecmp`.
- SBI (Supervisor Binary Interface) is used for early console output if no UART is detected.

---

## Error Handling Conventions

All HAL functions that can fail return `hal_status_t`. Callers should always check the return value:

```c
hal_status_t ret = hal_irq_register(irq, handler, data);
if (ret != HAL_OK) {
    hal_console_printf("ERROR: Failed to register IRQ %u: %d\n", irq, ret);
    return ret;
}
```

Functions that cannot fail (e.g., `hal_cpu_halt()`, `hal_console_putc()`) return `void`.

---

*AlJefra OS v0.7.3 -- HAL Specification -- Built in Qatar by [Qatar IT](https://www.qatarit.com)*
