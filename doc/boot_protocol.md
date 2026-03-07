# AlJefra OS -- Boot Protocol

## Overview

Each architecture has a different boot path at the firmware and assembly level, but all three converge at the same point: `hal_init()` followed by `kernel_main()`. This document describes the complete boot sequence for x86-64, ARM64, and RISC-V 64, the HAL initialization order, the kernel main flow, and the AI bootstrap process.

---

## Architecture-Specific Boot Paths

### x86-64: BIOS/UEFI -> Bootloader -> boot.asm -> hal_init.c -> main.c

#### Firmware Stage

The x86-64 build supports both legacy BIOS and UEFI boot:

- **BIOS:** The first sector of the disk image contains a VBR (Volume Boot Record) that loads the Pure64 bootloader. Pure64 switches the CPU from real mode to 64-bit long mode, enables A20, sets up an initial GDT and IDT, identity-maps the first 4 GB of memory, and jumps to the kernel entry point.
- **UEFI:** A UEFI application in the EFI System Partition loads the kernel ELF binary directly into memory, sets up long mode, and transfers control to the entry point.

In both cases, the CPU is in 64-bit long mode with paging enabled when the kernel receives control.

#### Assembly Stage (`arch/x86_64/boot.asm`)

Written in NASM Intel syntax. Responsibilities:

1. Receive control from Pure64/GRUB at the entry point `_start`.
2. Set up a 64 KB kernel stack at a known address.
3. Clear the BSS section (zero all uninitialized data).
4. Call `hal_init()` (C function).

```nasm
; arch/x86_64/boot.asm (simplified)
[BITS 64]
SECTION .text
GLOBAL _start
EXTERN hal_init

_start:
    ; Set up kernel stack
    mov rsp, kernel_stack_top

    ; Clear BSS
    xor rax, rax
    mov rdi, __bss_start
    mov rcx, __bss_size
    shr rcx, 3              ; divide by 8 (qwords)
    rep stosq

    ; Jump to C
    call hal_init
    jmp $                   ; should never return

SECTION .bss
ALIGN 16
kernel_stack:
    resb 65536              ; 64 KB stack
kernel_stack_top:
```

#### HAL Init (`arch/x86_64/hal_init.c`)

```c
void hal_init(void) {
    hal_console_init();     // VGA text mode + COM1 serial
    hal_cpu_init();         // CPUID, enable SSE/AVX, calibrate TSC
    hal_mmu_init();         // Set up PML4 page tables
    hal_irq_init();         // Set up IDT, configure I/O APIC
    hal_timer_init(1000);   // Program HPET at 1000 Hz
    // Bus scan and SMP init happen in kernel_main()
    kernel_main();
}
```

**Specific x86-64 initialization details:**

| Subsystem | Implementation |
|-----------|---------------|
| Console | VGA text buffer at `0xB8000`, COM1 serial at I/O port `0x3F8` (115200 baud, 8N1) |
| CPU | CPUID leaf enumeration, enable SSE2/AVX if present, calibrate TSC against PIT |
| MMU | PML4 at a static address, identity-map first 4 GB, map kernel to `0xFFFF800000000000` |
| Interrupts | 256-entry IDT, remap PIC to vectors 32-47, configure I/O APIC from ACPI MADT |
| Timer | HPET preferred (ACPI HPET table), PIT 8254 channel 0 as fallback |

---

### ARM64: UEFI/DTB -> boot.S -> hal_init.c -> main.c

#### Firmware Stage

ARM64 platforms typically boot via:

- **UEFI:** The firmware loads the kernel image (a flat binary or PE/COFF stub) to a specified address and passes a pointer to the UEFI System Table. The kernel extracts the DeviceTree (DTB) pointer from UEFI configuration tables.
- **Direct DTB:** On some SBCs (e.g., Raspberry Pi 4 with UEFI firmware), the firmware places a DTB at a known address and jumps directly to the kernel.

The CPU is in AArch64 EL1 (or EL2) with MMU off when the kernel receives control.

#### Assembly Stage (`arch/aarch64/boot.S`)

Written in GNU Assembler (GAS) ARM syntax. Responsibilities:

1. Receive control at `_start` with X0 = DTB pointer.
2. Save the DTB pointer for later use.
3. Set up the kernel stack (SP_EL1).
4. Clear the BSS section.
5. Branch to `hal_init()`.

```asm
// arch/aarch64/boot.S (simplified)
.section .text
.global _start

_start:
    // x0 = DTB pointer from firmware
    adr x1, _dtb_ptr
    str x0, [x1]

    // Set up kernel stack
    ldr x1, =kernel_stack_top
    mov sp, x1

    // Clear BSS
    ldr x1, =__bss_start
    ldr x2, =__bss_size
    cbz x2, .bss_done
.bss_loop:
    str xzr, [x1], #8
    subs x2, x2, #8
    b.gt .bss_loop
.bss_done:

    // Jump to C
    bl hal_init
    b .                      // should never return

.section .bss
.align 16
kernel_stack:
    .space 65536             // 64 KB stack
kernel_stack_top:

.section .data
.global _dtb_ptr
_dtb_ptr:
    .quad 0
```

#### HAL Init (`arch/aarch64/hal_init.c`)

```c
void hal_init(void) {
    hal_console_init();     // PL011 UART (base from DTB)
    hal_cpu_init();         // Read MIDR_EL1, enable caches
    hal_mmu_init();         // Set up Sv48 page tables, enable MMU
    hal_irq_init();         // Initialize GICv2/v3 distributor + CPU interface
    hal_timer_init(1000);   // Configure Generic Timer at 1000 Hz
    kernel_main();
}
```

**Specific ARM64 initialization details:**

| Subsystem | Implementation |
|-----------|---------------|
| Console | PL011 UART, base address discovered from DTB `/chosen/stdout-path` |
| CPU | Read MIDR_EL1 for CPU identification, enable I-cache and D-cache |
| MMU | TTBR0_EL1 for user space, TTBR1_EL1 for kernel; Sv48 (4-level) page tables |
| Interrupts | GICv3 preferred (GICD, GICR, ICC registers), GICv2 fallback (GICD, GICC) |
| Timer | Generic Timer: read frequency from `CNTFRQ_EL0`, program `CNTP_TVAL_EL0` |

---

### RISC-V 64: SBI/UEFI -> boot.S -> hal_init.c -> main.c

#### Firmware Stage

RISC-V 64 platforms boot via:

- **SBI (OpenSBI):** The M-mode firmware (OpenSBI) initializes the platform, sets up S-mode, and jumps to the kernel entry point with `a0` = hart ID and `a1` = DTB pointer.
- **UEFI:** Some RISC-V platforms provide UEFI firmware. The boot path is similar to ARM64 UEFI.

The CPU is in S-mode (supervisor mode) with MMU off when the kernel receives control.

#### Assembly Stage (`arch/riscv64/boot.S`)

Written in GNU Assembler (GAS) RISC-V syntax. Responsibilities:

1. Receive control at `_start` with `a0` = hart ID, `a1` = DTB pointer.
2. Only the bootstrap hart (hart 0) continues; secondary harts park (WFI loop).
3. Save DTB pointer.
4. Set up the kernel stack.
5. Clear the BSS section.
6. Jump to `hal_init()`.

```asm
# arch/riscv64/boot.S (simplified)
.section .text
.global _start

_start:
    # a0 = hart ID, a1 = DTB pointer
    # Only hart 0 boots; others park
    bnez a0, .park

    # Save DTB pointer
    la t0, _dtb_ptr
    sd a1, 0(t0)

    # Set up kernel stack
    la sp, kernel_stack_top

    # Clear BSS
    la t0, __bss_start
    la t1, __bss_end
.bss_loop:
    bge t0, t1, .bss_done
    sd zero, 0(t0)
    addi t0, t0, 8
    j .bss_loop
.bss_done:

    # Jump to C
    call hal_init

.park:
    wfi
    j .park

.section .bss
.align 16
kernel_stack:
    .space 65536             # 64 KB stack
kernel_stack_top:

.section .data
.global _dtb_ptr
_dtb_ptr:
    .quad 0
```

#### HAL Init (`arch/riscv64/hal_init.c`)

```c
void hal_init(void) {
    hal_console_init();     // SBI console or 16550 UART
    hal_cpu_init();         // Read marchid, mvendorid CSRs
    hal_mmu_init();         // Set up Sv39 page tables, write satp CSR
    hal_irq_init();         // Configure PLIC thresholds and priorities
    hal_timer_init(1000);   // Program CLINT mtimecmp
    kernel_main();
}
```

**Specific RISC-V initialization details:**

| Subsystem | Implementation |
|-----------|---------------|
| Console | SBI `ecall` for putchar (early), 16550 UART when available (DTB discovery) |
| CPU | Read `marchid`, `mvendorid`, `mimpid` CSRs for identification |
| MMU | Sv39 (3-level, 39-bit VA, 56-bit PA), written to `satp` CSR with ASID 0 |
| Interrupts | PLIC with per-context threshold = 0, priorities = 1 for all enabled sources |
| Timer | CLINT: read `mtime`, program `mtimecmp` for next tick; uses SBI timer call in S-mode |

---

## HAL Initialization Order

Regardless of architecture, the HAL subsystems are initialized in a strict order because later subsystems depend on earlier ones:

```
Step 1: hal_console_init()
        Needed immediately for debug/panic output during the rest of init.

Step 2: hal_cpu_init()
        CPU identification and feature enablement. MMU and IRQ code
        may need to know CPU features (e.g., available page sizes).

Step 3: hal_mmu_init()
        Sets up kernel page tables and enables the MMU. All subsequent
        MMIO accesses require virtual address mappings.

Step 4: hal_irq_init()
        Programs the interrupt controller. Timer and bus scan generate
        interrupts, so the IRQ system must be ready first.

Step 5: hal_timer_init(1000)
        Programs the system timer at 1000 Hz (1 ms tick). The scheduler
        and delay functions depend on this.

Step 6: hal_bus_scan(callback)       [called from kernel_main]
        Enumerates PCI/USB/platform devices. Requires MMIO (for PCI
        ECAM) and IRQs (for device discovery interrupts).

Step 7: hal_smp_init()              [called from kernel_main]
        Brings up secondary CPUs/cores/harts. Done last because the
        secondary CPUs need all subsystems already initialized.
```

---

## Kernel Main Flow

After `hal_init()` completes, control passes to `kernel_main()` in `kernel/main.c`. The flow is the same on all architectures:

```c
void kernel_main(void) {
    /* 1. Print boot banner */
    hal_console_printf("\n");
    hal_console_printf("=======================================\n");
    hal_console_printf("  AlJefra OS v0.7.2\n");
    hal_console_printf("  The AI-Native Operating System\n");
    hal_console_printf("  Architecture: %s\n", arch_name());
    hal_console_printf("  Built in Qatar\n");
    hal_console_printf("=======================================\n\n");

    /* 2. Bus enumeration -- discover all connected hardware */
    hal_console_printf("[BOOT] Scanning buses...\n");
    hal_bus_scan(device_found_callback);

    /* 3. Load built-in drivers for discovered devices */
    hal_console_printf("[BOOT] Loading built-in drivers...\n");
    load_builtin_drivers();

    /* 4. Initialize the scheduler */
    hal_console_printf("[BOOT] Starting scheduler...\n");
    sched_init();

    /* 5. Bring up SMP */
    hal_console_printf("[BOOT] Starting secondary CPUs...\n");
    hal_smp_init();
    hal_console_printf("[BOOT] %u CPUs online\n", hal_smp_cpu_count());

    /* 6. Network initialization */
    hal_console_printf("[BOOT] Initializing network...\n");
    net_init();         // Initialize the network stack
    dhcp_discover();    // Obtain IP address via DHCP

    /* 7. AI Bootstrap */
    hal_console_printf("[BOOT] Starting AI bootstrap...\n");
    ai_bootstrap();

    /* 8. System ready */
    hal_console_printf("[BOOT] System ready.\n\n");

    /* 9. Start the GUI desktop (if display is available) */
    if (display_available()) {
        gui_start();
    }

    /* 10. Enter the idle loop */
    while (1) {
        hal_cpu_halt();
    }
}
```

---

## AI Bootstrap Protocol

The AI bootstrap (`ai/bootstrap.c`) is the process by which AlJefra OS automatically configures itself for the hardware it is running on. It runs after the network is up.

### Step-by-Step Flow

```
+--------------------------------------------------------------------+
| Step 1: Build Hardware Manifest                                    |
|                                                                    |
|   Scan PCI bus    -> list of (vendor_id, device_id, class, sub)    |
|   Scan USB bus    -> list of (vendor_id, product_id, class)        |
|   Scan platform   -> list of compatible strings (from DTB)         |
|   Read CPU info   -> architecture, core count, features            |
|   Read memory     -> total RAM size                                |
|                                                                    |
|   Output: JSON manifest                                            |
+--------------------------------------------------------------------+
         |
         v
+--------------------------------------------------------------------+
| Step 2: Network Prerequisites                                     |
|                                                                    |
|   Verify DHCP lease is active (IP, gateway, DNS)                   |
|   Resolve store.aljefra.com via DNS                                |
+--------------------------------------------------------------------+
         |
         v
+--------------------------------------------------------------------+
| Step 3: Connect to AlJefra Driver Marketplace                     |
|                                                                    |
|   POST https://store.aljefra.com/api/v1/match                     |
|   Body: { "arch": "x86_64", "devices": [...], "os_version": "1.0" }|
|                                                                    |
|   Response: [                                                      |
|     { "name": "nvidia-rtx4090",                                    |
|       "version": "1.2.0",                                          |
|       "url": "https://store.aljefra.com/drv/nvidia-rtx4090.ajdrv", |
|       "sha256": "a1b2c3...",                                       |
|       "signature": "ed25519:..." },                                |
|     ...                                                            |
|   ]                                                                |
+--------------------------------------------------------------------+
         |
         v
+--------------------------------------------------------------------+
| Step 4: Download Drivers                                           |
|                                                                    |
|   For each driver in the response:                                 |
|     - HTTP GET the .ajdrv file                                     |
|     - Verify SHA-256 hash                                          |
|     - Verify Ed25519 signature against AlJefra public key          |
+--------------------------------------------------------------------+
         |
         v
+--------------------------------------------------------------------+
| Step 5: Load and Initialize                                        |
|                                                                    |
|   For each verified .ajdrv file:                                   |
|     - Map the driver binary into kernel memory                     |
|     - Relocate symbols                                             |
|     - Call driver_ops_t.init()                                     |
|     - Register with the kernel driver manager                      |
|     - Log success or failure                                       |
+--------------------------------------------------------------------+
         |
         v
+--------------------------------------------------------------------+
| Step 6: Report                                                     |
|                                                                    |
|   Print summary: N drivers loaded, M failed                        |
|   POST telemetry to store (opt-in): success/fail per device        |
+--------------------------------------------------------------------+
```

### Manifest Format

```json
{
  "os_version": "1.0",
  "arch": "x86_64",
  "cpu": {
    "vendor": "GenuineIntel",
    "cores": 4,
    "features": ["sse2", "avx", "avx2"]
  },
  "memory_mb": 8192,
  "devices": [
    {
      "bus": "pci",
      "vendor_id": "0x10DE",
      "device_id": "0x2684",
      "class": "0x03",
      "subclass": "0x00"
    },
    {
      "bus": "usb",
      "vendor_id": "0x046D",
      "product_id": "0xC534",
      "class": "0x03"
    }
  ]
}
```

### Fallback Behavior

If the network is unavailable or the marketplace cannot be reached:

1. The system continues with built-in drivers only.
2. A background task retries the AI bootstrap every 30 seconds.
3. When connectivity is established, the bootstrap runs and loads any missing drivers.
4. The user is notified via the console or GUI that additional drivers have been loaded.

---

## Linker Script Details

Each architecture has its own linker script (`arch/<arch>/linker.ld`) that defines the memory layout of the kernel image.

### Common Sections

All linker scripts define the following sections in this order:

| Section | Content | Alignment |
|---------|---------|-----------|
| `.text` | Executable code | 4096 (page-aligned) |
| `.rodata` | Read-only data, string literals | 4096 |
| `.data` | Initialized global/static variables | 4096 |
| `.bss` | Uninitialized global/static variables (zeroed) | 4096 |

### Exported Symbols

The linker script exports symbols that the boot assembly and kernel use:

| Symbol | Meaning |
|--------|---------|
| `_start` | Entry point (first instruction executed) |
| `__text_start`, `__text_end` | Bounds of the `.text` section |
| `__rodata_start`, `__rodata_end` | Bounds of the `.rodata` section |
| `__data_start`, `__data_end` | Bounds of the `.data` section |
| `__bss_start`, `__bss_end` | Bounds of the `.bss` section |
| `__bss_size` | Size of the `.bss` section in bytes |
| `__kernel_end` | End of the entire kernel image |

### Architecture-Specific Base Addresses

| Architecture | Kernel Load Address | Notes |
|-------------|-------------------|-------|
| x86-64 | `0x100000` (1 MB) | Above legacy BIOS area |
| ARM64 | `0x40080000` | QEMU `virt` machine convention |
| RISC-V 64 | `0x80200000` | After OpenSBI firmware |

---

## QEMU Boot Commands

### x86-64

```bash
qemu-system-x86_64 \
  -drive file=build/aljefra-x86_64.img,format=raw \
  -m 256M \
  -smp 2 \
  -device e1000,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::8080-:80 \
  -serial stdio \
  -no-reboot
```

### ARM64

```bash
qemu-system-aarch64 \
  -M virt \
  -cpu cortex-a72 \
  -drive file=build/aljefra-aarch64.img,format=raw,if=none,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -m 256M \
  -smp 2 \
  -device virtio-net-device,netdev=net0 \
  -netdev user,id=net0 \
  -nographic
```

### RISC-V 64

```bash
qemu-system-riscv64 \
  -M virt \
  -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
  -drive file=build/aljefra-riscv64.img,format=raw,if=none,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -m 256M \
  -smp 2 \
  -device virtio-net-device,netdev=net0 \
  -netdev user,id=net0 \
  -nographic
```

### Debugging with GDB

Add `-s -S` to any QEMU command to start a GDB server on port 1234 and pause at the first instruction:

```bash
# In terminal 1:
qemu-system-x86_64 -s -S -drive file=build/aljefra-x86_64.img,format=raw -m 256M

# In terminal 2:
gdb -ex "target remote :1234" -ex "symbol-file build/aljefra-x86_64.elf"
```

---

## Boot Timeline (Approximate)

| Phase | Duration | Notes |
|-------|----------|-------|
| Firmware (BIOS/UEFI/SBI) | 0.5 - 2.0 s | Platform-dependent |
| Bootloader (Pure64/GRUB) | 0.1 - 0.5 s | |
| Assembly boot (boot.S) | < 1 ms | Stack setup, BSS clear |
| HAL initialization | 10 - 50 ms | Console, CPU, MMU, IRQ, Timer |
| Bus enumeration | 5 - 20 ms | PCI scan |
| Built-in driver load | 10 - 50 ms | e1000, PS/2, VGA |
| DHCP | 1 - 5 s | Network-dependent |
| AI bootstrap | 2 - 30 s | Depends on driver count and network speed |
| **Total to system ready** | **~4 - 90 s** | **Network speed is the main variable** |

---

*AlJefra OS v0.7.2 -- Boot Protocol -- Built in Qatar by [Qatar IT](https://www.qatarit.com)*
