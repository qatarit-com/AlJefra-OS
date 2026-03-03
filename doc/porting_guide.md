# AlJefra OS Porting Guide

## Overview

This guide describes how to port AlJefra OS to a new CPU architecture. AlJefra OS uses a
Hardware Abstraction Layer (HAL) that isolates architecture-specific code from the
portable kernel. Porting the OS to a new architecture requires implementing the HAL
interface for that architecture, writing a boot stub, creating a linker script, and
updating the build system.

The existing ports serve as reference implementations:
- `arch/x86_64/` -- AMD64 / Intel 64-bit
- `arch/aarch64/` -- ARM 64-bit (ARMv8-A)
- `arch/riscv64/` -- RISC-V 64-bit (RV64GC)

---

## Prerequisites

Before starting a port, ensure you have:

- A cross-compilation toolchain for the target architecture (GCC or Clang).
- QEMU or another emulator that supports the target architecture.
- Familiarity with the target architecture's privilege levels, memory management, and
  interrupt handling.
- A copy of the target architecture's reference manual.

---

## Step 1: Create the Architecture Directory

Create a new directory under `arch/` for your architecture:

```bash
mkdir -p arch/<newarch>
```

Replace `<newarch>` with a short identifier for the architecture (e.g., `loongarch64`,
`mips64`, `powerpc64`).

This directory will contain all architecture-specific source files:

```
arch/<newarch>/
    boot.S          # Assembly boot stub
    hal_init.c      # HAL initialization dispatcher
    cpu.c           # CPU mode setup, feature detection
    interrupt.c     # Interrupt controller driver
    timer.c         # System timer driver
    bus.c           # Bus enumeration (PCI, etc.)
    io.c            # Port/MMIO I/O primitives
    mmu.c           # Page table setup and management
    smp.c           # Multi-core startup
    console.c       # Early console output (UART, etc.)
    linker.ld       # Linker script
```

---

## Step 2: Implement boot.S

The boot stub is the first code that executes when the kernel starts. It is written in
assembly because the C runtime is not yet available. The boot stub must:

1. **Set up the CPU mode**: Switch to the appropriate privilege level for kernel
   execution.
   - x86-64: Switch from real mode to long mode (enable paging, set up GDT).
   - aarch64: Ensure EL1 (or drop from EL2/EL3 to EL1).
   - riscv64: Ensure S-mode (OpenSBI runs in M-mode and drops to S-mode).

2. **Set up the stack**: Point the stack pointer to a valid memory region.
   ```asm
   # Example (generic):
   la  sp, _stack_top    # Load stack top address
   ```

3. **Zero the BSS section**: Clear uninitialized global variables.
   ```asm
   # Zero from _bss_start to _bss_end
   ```

4. **Jump to C code**: Call the `kernel_main()` function (or `hal_init()` if your port
   initializes the HAL before entering the common kernel).
   ```asm
   call kernel_main      # Never returns
   ```

### Architecture-Specific Considerations

| Architecture | Boot Environment               | Stack Setup                    |
|-------------|--------------------------------|--------------------------------|
| x86_64      | Multiboot2 (GRUB), real mode   | Set RSP to `_stack_top`        |
| aarch64     | DTB pointer in x0, EL1/EL2    | Set SP to `_stack_top`         |
| riscv64     | Hart ID in a0, DTB in a1, S-mode | Set SP to `_stack_top`      |
| (your arch) | Document boot protocol here    | Set SP appropriately           |

### Tips

- Keep `boot.S` as minimal as possible. Do only what cannot be done in C.
- Save any boot parameters (DTB pointer, hart ID, etc.) in registers or global
  variables before calling C code.
- Ensure the stack is 16-byte aligned (required by most ABIs).

---

## Step 3: Implement hal_init.c

The `hal_init()` function is the main initialization dispatcher for the architecture.
It is called from `kernel_main()` and must initialize all hardware subsystems in the
correct order.

### Initialization Order

The initialization order is critical. Later subsystems depend on earlier ones:

```c
void hal_init(void) {
    // 1. Console: Must be first so we can print debug messages
    console_init();

    // 2. CPU: Set up CPU features, privilege levels, control registers
    cpu_init();

    // 3. MMU: Set up page tables and enable virtual memory
    mmu_init();

    // 4. Interrupts: Set up interrupt controller and exception handlers
    interrupt_init();

    // 5. Timer: Set up system timer for scheduling and timeouts
    timer_init();

    // 6. Bus: Enumerate PCI and other buses, detect devices
    bus_init();

    // 7. SMP: Start secondary CPU cores (if available)
    smp_init();
}
```

### Why This Order?

| Step       | Reason                                                         |
|------------|----------------------------------------------------------------|
| console    | All subsequent steps print diagnostic messages                 |
| cpu        | MMU setup may require CPU feature flags                        |
| mmu        | Interrupts require valid page tables for handler addresses     |
| interrupts | Timer generates interrupts; must have handler installed first  |
| timer      | Bus enumeration may need timeouts                              |
| bus        | SMP startup may need to discover inter-processor interrupts    |
| smp        | Secondary cores can only start after all shared state is ready |

---

## Step 4: Implement HAL Modules

Each HAL module implements a specific hardware interface. The functions listed below must
be implemented for every new architecture. The portable kernel calls these functions
through the HAL interface.

### cpu.c -- CPU Initialization

```c
// Initialize CPU features, control registers, privilege level
void cpu_init(void);

// Halt the CPU (low-power wait for interrupt)
void cpu_halt(void);

// Disable all interrupts and return previous state
uint64_t cpu_disable_interrupts(void);

// Restore interrupt state
void cpu_restore_interrupts(uint64_t state);

// Read a CPU identification value (vendor, model, features)
uint64_t cpu_read_id(void);
```

### interrupt.c -- Interrupt Controller

```c
// Initialize the interrupt controller (APIC, GIC, PLIC, etc.)
void interrupt_init(void);

// Register a handler for a specific interrupt number
void interrupt_register(uint32_t irq, void (*handler)(void *), void *context);

// Enable a specific interrupt
void interrupt_enable(uint32_t irq);

// Disable a specific interrupt
void interrupt_disable(uint32_t irq);

// Acknowledge / end-of-interrupt
void interrupt_eoi(uint32_t irq);
```

### timer.c -- System Timer

```c
// Initialize the system timer
void timer_init(void);

// Get current time in nanoseconds since boot
uint64_t timer_get_ns(void);

// Set a one-shot timer to fire after `ns` nanoseconds
void timer_set_oneshot(uint64_t ns);

// Get timer frequency in Hz
uint64_t timer_get_frequency(void);
```

### bus.c -- Bus Enumeration

```c
// Initialize and enumerate buses (PCI, VirtIO MMIO, etc.)
void bus_init(void);

// Read a PCI configuration register
uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func,
                         uint8_t offset);

// Write a PCI configuration register
void pci_config_write(uint8_t bus, uint8_t device, uint8_t func,
                      uint8_t offset, uint32_t value);

// Get the number of detected PCI devices
uint32_t pci_device_count(void);

// Get information about a detected PCI device
void pci_device_info(uint32_t index, uint16_t *vendor_id, uint16_t *device_id,
                     uint8_t *class, uint8_t *subclass);
```

### io.c -- I/O Primitives

```c
// Read/write to I/O ports (x86-specific, may be no-ops on other architectures)
uint8_t  io_inb(uint16_t port);
uint16_t io_inw(uint16_t port);
uint32_t io_ind(uint16_t port);
void     io_outb(uint16_t port, uint8_t value);
void     io_outw(uint16_t port, uint16_t value);
void     io_outd(uint16_t port, uint32_t value);

// Memory-mapped I/O (all architectures)
uint8_t  mmio_read8(volatile void *addr);
uint16_t mmio_read16(volatile void *addr);
uint32_t mmio_read32(volatile void *addr);
uint64_t mmio_read64(volatile void *addr);
void     mmio_write8(volatile void *addr, uint8_t value);
void     mmio_write16(volatile void *addr, uint16_t value);
void     mmio_write32(volatile void *addr, uint32_t value);
void     mmio_write64(volatile void *addr, uint64_t value);
```

### mmu.c -- Memory Management Unit

```c
// Initialize page tables and enable the MMU
void mmu_init(void);

// Map a virtual address to a physical address with given flags
void mmu_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a virtual address
void mmu_unmap_page(uint64_t virt);

// Translate a virtual address to a physical address
uint64_t mmu_virt_to_phys(uint64_t virt);

// Flush TLB for a specific virtual address
void mmu_flush_tlb(uint64_t virt);

// Flush the entire TLB
void mmu_flush_tlb_all(void);
```

### smp.c -- Symmetric Multiprocessing

```c
// Initialize SMP and start secondary cores
void smp_init(void);

// Get the current CPU/hart ID
uint32_t smp_cpu_id(void);

// Get the number of available CPUs/harts
uint32_t smp_cpu_count(void);

// Send an inter-processor interrupt (IPI) to a specific CPU
void smp_send_ipi(uint32_t cpu_id);
```

### console.c -- Early Console

```c
// Initialize the early console (UART, VGA text mode, etc.)
void console_init(void);

// Write a single character to the console
void console_putchar(char c);

// Write a string to the console
void console_puts(const char *str);

// Read a character from the console (blocking)
char console_getchar(void);
```

---

## Step 5: Create linker.ld

The linker script defines the memory layout of the kernel binary. It specifies where
each section (.text, .rodata, .data, .bss) is placed in memory and defines symbols that
the boot stub and kernel reference.

### Template

```ld
/* arch/<newarch>/linker.ld */

ENTRY(_start)

SECTIONS
{
    /* Set the load address appropriate for your architecture */
    . = KERNEL_LOAD_ADDRESS;

    .text : {
        _text_start = .;
        *(.text.boot)       /* Boot stub goes first */
        *(.text .text.*)
        _text_end = .;
    }

    .rodata : ALIGN(4096) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    }

    .data : ALIGN(4096) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss : ALIGN(4096) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* Kernel stack */
    . = ALIGN(4096);
    _stack_bottom = .;
    . += KERNEL_STACK_SIZE;     /* e.g., 16384 (16 KB) */
    _stack_top = .;

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
```

### Load Addresses by Architecture

| Architecture | Load Address   | Notes                                     |
|-------------|----------------|-------------------------------------------|
| x86_64      | 0x100000       | 1 MB (above legacy BIOS area)             |
| aarch64     | 0x40080000     | After DTB in RAM on QEMU virt             |
| riscv64     | 0x80200000     | After OpenSBI on QEMU virt                |
| (your arch) | TBD            | Depends on firmware and boot protocol     |

---

## Step 6: Update the Makefile

Add your architecture as a build target in the root `Makefile`.

### Required Changes

1. **Add architecture detection**:

```makefile
ifeq ($(ARCH),<newarch>)
    CC      = <newarch>-linux-gnu-gcc
    AS      = <newarch>-linux-gnu-as
    LD      = <newarch>-linux-gnu-ld
    OBJCOPY = <newarch>-linux-gnu-objcopy
    CFLAGS += -march=<cpu_march> -mabi=<abi>
    ASFLAGS += <arch-specific-flags>
    LDFLAGS += -T arch/<newarch>/linker.ld
    ARCH_SRCS = $(wildcard arch/<newarch>/*.c)
    ARCH_ASM  = $(wildcard arch/<newarch>/*.S)
    QEMU      = qemu-system-<newarch>
    QEMU_FLAGS = -machine <machine> -cpu <cpu> -m 256M -nographic
endif
```

2. **Add to the `all-arch` target**:

```makefile
all-arch: x86_64 aarch64 riscv64 <newarch>
```

3. **Add QEMU run target**:

```makefile
run-<newarch>: build/<newarch>/kernel.elf
    $(QEMU) $(QEMU_FLAGS) -kernel build/<newarch>/kernel.elf
```

### Toolchain Variables Reference

| Variable   | Purpose                                | Example (aarch64)            |
|-----------|----------------------------------------|------------------------------|
| `CC`      | C compiler                             | `aarch64-linux-gnu-gcc`      |
| `AS`      | Assembler                              | `aarch64-linux-gnu-as`       |
| `LD`      | Linker                                 | `aarch64-linux-gnu-ld`       |
| `OBJCOPY` | Binary format converter                | `aarch64-linux-gnu-objcopy`  |
| `CFLAGS`  | C compiler flags                       | `-march=armv8-a -mabi=lp64`  |
| `ASFLAGS` | Assembler flags                        | (architecture-specific)      |
| `LDFLAGS` | Linker flags (includes linker script)  | `-T arch/aarch64/linker.ld`  |

---

## Step 7: Test on QEMU

QEMU is the primary testing platform for AlJefra OS. Every port must boot successfully
on QEMU before being considered complete.

### Build

```bash
make ARCH=<newarch>
```

### Run

```bash
make run-<newarch>
```

Or manually:

```bash
qemu-system-<newarch> \
    -machine <machine_type> \
    -cpu <cpu_model> \
    -m 256M \
    -nographic \
    -kernel build/<newarch>/kernel.elf
```

### What to Verify

1. **Console output**: The kernel should print initialization messages to the serial
   console.
2. **No exceptions/faults**: The kernel should not crash or triple-fault during boot.
3. **HAL initialization**: All HAL modules should report successful initialization.
4. **PCI enumeration**: The kernel should detect QEMU virtual devices.
5. **Timer**: The system timer should produce periodic interrupts.
6. **SMP** (if applicable): Secondary cores should start and report in.

---

## HAL Function Checklist

Use this checklist to track your progress. Every function must be implemented (even if
as a stub that prints "not implemented") for the kernel to link successfully.

### Console (console.c)

- [ ] `console_init()`
- [ ] `console_putchar()`
- [ ] `console_puts()`
- [ ] `console_getchar()`

### CPU (cpu.c)

- [ ] `cpu_init()`
- [ ] `cpu_halt()`
- [ ] `cpu_disable_interrupts()`
- [ ] `cpu_restore_interrupts()`
- [ ] `cpu_read_id()`

### MMU (mmu.c)

- [ ] `mmu_init()`
- [ ] `mmu_map_page()`
- [ ] `mmu_unmap_page()`
- [ ] `mmu_virt_to_phys()`
- [ ] `mmu_flush_tlb()`
- [ ] `mmu_flush_tlb_all()`

### Interrupts (interrupt.c)

- [ ] `interrupt_init()`
- [ ] `interrupt_register()`
- [ ] `interrupt_enable()`
- [ ] `interrupt_disable()`
- [ ] `interrupt_eoi()`

### Timer (timer.c)

- [ ] `timer_init()`
- [ ] `timer_get_ns()`
- [ ] `timer_set_oneshot()`
- [ ] `timer_get_frequency()`

### Bus (bus.c)

- [ ] `bus_init()`
- [ ] `pci_config_read()`
- [ ] `pci_config_write()`
- [ ] `pci_device_count()`
- [ ] `pci_device_info()`

### I/O (io.c)

- [ ] `io_inb()` / `io_inw()` / `io_ind()`
- [ ] `io_outb()` / `io_outw()` / `io_outd()`
- [ ] `mmio_read8()` / `mmio_read16()` / `mmio_read32()` / `mmio_read64()`
- [ ] `mmio_write8()` / `mmio_write16()` / `mmio_write32()` / `mmio_write64()`

### SMP (smp.c)

- [ ] `smp_init()`
- [ ] `smp_cpu_id()`
- [ ] `smp_cpu_count()`
- [ ] `smp_send_ipi()`

---

## Tips and Best Practices

### Start with Console Output

The first thing to get working is `console_putchar()`. Once you can print characters to
a serial port, you can debug everything else. On most QEMU machines, this means writing
to a UART register:

```c
#define UART_BASE 0x.........   // Architecture/machine-specific
void console_putchar(char c) {
    *(volatile char *)UART_BASE = c;
}
```

### Then CPU Initialization

Get `cpu_init()` working next. This typically means setting up control registers,
enabling any required CPU features, and ensuring you are in the correct privilege level.

### Then MMU

Page table setup is usually the most complex part of a port. Study the existing
implementations carefully:

- `arch/x86_64/mmu.c` -- 4-level page tables (PML4)
- `arch/aarch64/mmu.c` -- 4-level translation tables
- `arch/riscv64/mmu.c` -- Sv39/Sv48 page tables

Key patterns that are common across architectures:
- Allocate a root page table (zeroed memory).
- Identity-map the kernel physical address range.
- Map the kernel into the higher-half virtual address range.
- Map MMIO regions as uncacheable.
- Enable the MMU (write to the appropriate control register).

### Then Interrupts

Once the MMU is up, set up the interrupt controller. Each architecture uses a different
controller:

| Architecture | Controller | Key Registers                              |
|-------------|------------|--------------------------------------------|
| x86_64      | APIC       | APIC base (0xFEE00000), ICR, LVT          |
| aarch64     | GICv2      | GICD (distributor), GICC (CPU interface)   |
| riscv64     | PLIC       | Priority, pending, enable, claim/complete  |

### Use Existing Ports as Reference

The three existing ports follow identical patterns. When implementing a new module, open
the corresponding file from two or three existing ports side by side. The structure and
logic will be very similar; only the register addresses and instruction mnemonics differ.

### Stub Everything First

Before implementing any function fully, create stubs for all HAL functions so the kernel
links:

```c
void timer_init(void) {
    klog(KLOG_WARN, "<newarch>: timer_init() not yet implemented\n");
}
```

This lets you build and boot the kernel immediately, then implement functions one at a
time.

### Test Incrementally

After implementing each HAL module, build and boot on QEMU. Do not implement everything
at once and then try to boot -- you will have too many potential failure points.

Recommended implementation and testing order:

1. `boot.S` + `console.c` -- Boot and print "Hello" to serial.
2. `cpu.c` -- Initialize CPU, print feature flags.
3. `mmu.c` -- Enable paging, verify kernel still runs.
4. `interrupt.c` -- Set up exception handlers, trigger a test exception.
5. `timer.c` -- Set up periodic timer, verify interrupts fire.
6. `bus.c` -- Enumerate PCI, print detected devices.
7. `smp.c` -- Start secondary cores, have them print their IDs.

---

## Common Pitfalls

1. **Memory barriers**: Each architecture has different memory ordering. Make sure
   appropriate fence/barrier instructions are used after MMIO writes and before MMIO
   reads. x86 is strongly ordered; ARM and RISC-V are weakly ordered.

2. **Cache coherency**: DMA buffers may need explicit cache flush/invalidation on ARM64
   and RISC-V. x86 is hardware cache-coherent for DMA. See `memory_maps.md` for
   per-architecture DMA constraints.

3. **Endianness**: AlJefra OS assumes little-endian operation on all architectures. ARM
   and RISC-V can operate in big-endian mode; ensure your port configures the CPU for
   little-endian.

4. **Alignment**: Some architectures fault on unaligned memory access. Use
   `__attribute__((aligned(N)))` or manual alignment for structures accessed by hardware.

5. **Stack alignment**: Most ABIs require 16-byte stack alignment at function call
   boundaries. Misalignment causes subtle and hard-to-debug crashes.

---

## Reference: Existing Architecture Implementations

| File             | x86_64                   | aarch64                  | riscv64                  |
|------------------|--------------------------|--------------------------|--------------------------|
| Boot stub        | `arch/x86_64/boot.S`    | `arch/aarch64/boot.S`   | `arch/riscv64/boot.S`   |
| HAL init         | `arch/x86_64/hal_init.c`| `arch/aarch64/hal_init.c`| `arch/riscv64/hal_init.c`|
| CPU              | `arch/x86_64/cpu.c`     | `arch/aarch64/cpu.c`    | `arch/riscv64/cpu.c`    |
| Interrupts       | `arch/x86_64/interrupt.c`| `arch/aarch64/interrupt.c`| `arch/riscv64/interrupt.c`|
| Timer            | `arch/x86_64/timer.c`   | `arch/aarch64/timer.c`  | `arch/riscv64/timer.c`  |
| Bus              | `arch/x86_64/bus.c`     | `arch/aarch64/bus.c`    | `arch/riscv64/bus.c`    |
| I/O              | `arch/x86_64/io.c`      | `arch/aarch64/io.c`     | `arch/riscv64/io.c`     |
| MMU              | `arch/x86_64/mmu.c`     | `arch/aarch64/mmu.c`    | `arch/riscv64/mmu.c`    |
| SMP              | `arch/x86_64/smp.c`     | `arch/aarch64/smp.c`    | `arch/riscv64/smp.c`    |
| Console          | `arch/x86_64/console.c` | `arch/aarch64/console.c`| `arch/riscv64/console.c`|
| Linker script    | `arch/x86_64/linker.ld` | `arch/aarch64/linker.ld`| `arch/riscv64/linker.ld`|
