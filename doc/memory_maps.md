# AlJefra OS Memory Maps

## Overview

This document describes the physical and virtual memory layouts for all three
architectures supported by AlJefra OS: x86-64, ARM64 (aarch64), and RISC-V 64
(riscv64). Each architecture has its own page table format, address space layout, and
hardware-imposed constraints. Understanding these maps is essential for kernel
development, driver authoring, and porting work.

---

## x86-64 Memory Map

### Page Table Structure

x86-64 uses 4-level page tables with 48-bit virtual addresses (256 TB virtual address
space):

```
Level 4: PML4   (Page Map Level 4)              - 512 entries, each covers 512 GB
Level 3: PDPT   (Page Directory Pointer Table)  - 512 entries, each covers 1 GB
Level 2: PD     (Page Directory)                - 512 entries, each covers 2 MB
Level 1: PT     (Page Table)                    - 512 entries, each covers 4 KB
```

Each page table entry is 8 bytes. The CR3 register points to the physical address of
the PML4.

### Physical Memory Map

```
Address Range               Size        Description
---------------------------------------------------------------------------
0x0000_0000 - 0x0000_03FF  1 KB        Real Mode IVT (Interrupt Vector Table)
0x0000_0400 - 0x0000_04FF  256 B       BIOS Data Area (BDA)
0x0000_0500 - 0x0000_7BFF  ~29 KB      Conventional memory (free)
0x0000_7C00 - 0x0000_7DFF  512 B       Boot sector load address
0x0000_7E00 - 0x0007_FFFF  ~480 KB     Conventional memory (free)
0x0008_0000 - 0x0009_FFFF  128 KB      EBDA (Extended BIOS Data Area)
0x000A_0000 - 0x000B_FFFF  128 KB      VGA framebuffer (legacy)
0x000C_0000 - 0x000F_FFFF  256 KB      BIOS ROM area
---------------------------------------------------------------------------
0x0010_0000 - 0x00FF_FFFF  ~15 MB      Kernel load area (starts at 1 MB)
  0x0010_0000                           Kernel .text section start
  0x0010_0000 + text_size               Kernel .rodata section
  after .rodata                         Kernel .data section
  after .data                           Kernel .bss section
0x0100_0000 - ...          variable     Kernel heap / dynamic allocations
---------------------------------------------------------------------------
0x8000_0000 - 0xBFFF_FFFF  1 GB        MMIO region (PCIe BARs, device memory)
0xFEC0_0000                 4 KB        I/O APIC base
0xFED0_0000                 4 KB        HPET base
0xFEE0_0000                 4 KB        Local APIC base
0xFEF0_0000 - 0xFFFF_FFFF  ~16 MB      System ROM / firmware
```

### Virtual Memory Map

```
Virtual Address Range               Description
---------------------------------------------------------------------------
0x0000_0000_0000_0000 -             Direct-mapped physical memory
0x0000_0000_FFFF_FFFF               (lower 4 GB identity mapped 1:1)
                                    Used during early boot and for MMIO access.

0x0000_0001_0000_0000 -             Available for future user-space mapping
0x0000_7FFF_FFFF_FFFF               (not currently used; AlJefra has no user space yet)

--- canonical address hole ---       Non-canonical addresses (x86-64 gap)

0xFFFF_8000_0000_0000 -             Kernel direct map (higher-half mapping)
0xFFFF_FFFF_FFFF_FFFF               All physical memory remapped here.
  0xFFFF_8000_0010_0000             Kernel code (higher-half equivalent)
  0xFFFF_8000_8000_0000             MMIO region (higher-half mapped)
  0xFFFF_8000_FEE0_0000             Local APIC (higher-half mapped)
```

### Stack Layout (x86-64)

```
+---------------------------+ High address (stack top)
|       Guard Page          | 4 KB (unmapped, catches overflow)
+---------------------------+
|                           |
|     Kernel Stack          | 16 KB (4 pages)
|     (grows downward)      |
|                           |
+---------------------------+
|       Guard Page          | 4 KB (unmapped, catches underflow)
+---------------------------+ Low address
```

Each CPU core has its own kernel stack. On SMP systems, per-CPU stacks are allocated
during `smp_init()`.

---

## ARM64 (aarch64) Memory Map

### Page Table Structure

ARM64 uses a configurable page table hierarchy. AlJefra OS uses 4-level translation
tables with 4 KB granule (Sv48 equivalent), providing 48-bit virtual addresses:

```
Level 0: L0 Table  - 512 entries, each covers 512 GB
Level 1: L1 Table  - 512 entries, each covers 1 GB  (supports 1 GB block descriptors)
Level 2: L2 Table  - 512 entries, each covers 2 MB  (supports 2 MB block descriptors)
Level 3: L3 Table  - 512 entries, each covers 4 KB  (page descriptors)
```

TTBR0_EL1 holds the base of the user-space table (lower addresses).
TTBR1_EL1 holds the base of the kernel-space table (upper addresses).

### Physical Memory Map (QEMU virt machine)

```
Address Range               Size        Description
---------------------------------------------------------------------------
0x0000_0000 - 0x07FF_FFFF  128 MB      Flash memory (firmware, DTB)
0x0800_0000 - 0x0800_FFFF  64 KB       GICv2 Distributor (GICD)
0x0801_0000 - 0x0801_FFFF  64 KB       GICv2 CPU Interface (GICC)
0x0900_0000 - 0x0900_0FFF  4 KB        PL011 UART (serial console)
0x0901_0000 - 0x0901_0FFF  4 KB        RTC (PL031)
0x0A00_0000 - 0x0A00_01FF  512 B       Firmware configuration (fw_cfg)
0x0C00_0000 - 0x0CFF_FFFF  16 MB       Platform-level device MMIO
0x1000_0000 - 0x3EFF_FFFF  752 MB      PCIe MMIO window
0x3F00_0000 - 0x3FFF_FFFF  16 MB       PCIe PIO (Port I/O) window
---------------------------------------------------------------------------
0x4000_0000 - 0x4007_FFFF  512 KB      Kernel load area (RAM start)
  0x4000_0000                           DTB (Device Tree Blob) passed by firmware
  0x4008_0000                           Kernel entry point (_start)
  0x4008_0000 + text_size               .rodata section
  after .rodata                         .data section
  after .data                           .bss section
0x4100_0000 - ...          variable     Kernel heap / dynamic allocations
---------------------------------------------------------------------------
0x4000_0000 - 0xBFFF_FFFF  2 GB        Total RAM (QEMU virt default)
  (configurable via QEMU -m flag)
```

### Virtual Memory Map

```
Virtual Address Range               Description
---------------------------------------------------------------------------
0x0000_0000_0000_0000 -             User-space (TTBR0_EL1)
0x0000_FFFF_FFFF_FFFF               Currently unused; reserved for future use.

0xFFFF_0000_0000_0000 -             Kernel space (TTBR1_EL1)
0xFFFF_FFFF_FFFF_FFFF
  0xFFFF_0000_4000_0000             Kernel direct map of RAM
  0xFFFF_0000_4008_0000             Kernel code (.text)
  0xFFFF_0000_0800_0000             GIC registers (mapped)
  0xFFFF_0000_0900_0000             UART registers (mapped)
  0xFFFF_0000_1000_0000             PCIe MMIO (mapped)
```

### Stack Layout (aarch64)

```
+---------------------------+ High address (SP initial value)
|       Guard Page          | 4 KB (unmapped)
+---------------------------+
|                           |
|     Kernel Stack          | 16 KB (4 pages)
|     (grows downward)      |
|                           |
+---------------------------+
|       Guard Page          | 4 KB (unmapped)
+---------------------------+ Low address
```

The stack pointer (SP) is initialized to the top of the usable region. The ARM64 ABI
requires 16-byte stack alignment.

---

## RISC-V 64 (riscv64) Memory Map

### Page Table Structure

RISC-V 64 supports two paging modes. AlJefra OS uses Sv39 by default (3-level page
tables, 39-bit virtual addresses, 512 GB virtual address space) with Sv48 as an option
(4-level, 48-bit, 256 TB):

**Sv39 (default):**

```
Level 2: Root Table  - 512 entries, each covers 1 GB  (supports 1 GB megapages)
Level 1: Mid Table   - 512 entries, each covers 2 MB  (supports 2 MB megapages)
Level 0: Leaf Table  - 512 entries, each covers 4 KB  (standard pages)
```

**Sv48 (optional):**

```
Level 3: Root Table  - 512 entries, each covers 512 GB
Level 2: L2 Table    - 512 entries, each covers 1 GB
Level 1: L1 Table    - 512 entries, each covers 2 MB
Level 0: L0 Table    - 512 entries, each covers 4 KB
```

The `satp` CSR holds the page table root address and paging mode (Sv39=8, Sv48=9).

### Physical Memory Map (QEMU virt machine)

```
Address Range               Size        Description
---------------------------------------------------------------------------
0x0000_0000 - 0x000F_FFFF  1 MB        Debug ROM / test area
0x0010_0000 - 0x0010_0FFF  4 KB        Test finisher (QEMU virt power off)
0x0200_0000 - 0x0200_FFFF  64 KB       CLINT (Core Local Interruptor)
                                         - msip registers
                                         - mtimecmp registers
                                         - mtime register
0x0C00_0000 - 0x0FFF_FFFF  64 MB       PLIC (Platform-Level Interrupt Controller)
  0x0C00_0000                           Priority registers
  0x0C00_1000                           Pending bits
  0x0C00_2000                           Enable bits (per context)
  0x0C20_0000                           Threshold and claim/complete
0x1000_0000 - 0x1000_0FFF  4 KB        UART (NS16550A compatible)
0x1000_1000 - 0x1000_1FFF  4 KB        VirtIO MMIO device 0
0x1000_2000 - 0x1000_2FFF  4 KB        VirtIO MMIO device 1
  ... (up to 8 VirtIO devices)
0x3000_0000 - 0x3FFF_FFFF  256 MB      PCIe MMIO window
---------------------------------------------------------------------------
0x8000_0000 - 0x801F_FFFF  2 MB        OpenSBI firmware (M-mode)
0x8020_0000 - ...          variable     Kernel load address (S-mode entry)
  0x8020_0000                           Kernel _start / .text section
  0x8020_0000 + text_size               .rodata section
  after .rodata                         .data section
  after .data                           .bss section
0x8100_0000 - ...          variable     Kernel heap / dynamic allocations
---------------------------------------------------------------------------
0x8000_0000 - 0xFFFF_FFFF  2 GB        Total RAM (QEMU virt default)
  (configurable via QEMU -m flag)
```

### Virtual Memory Map (Sv39)

```
Virtual Address Range               Description
---------------------------------------------------------------------------
0x0000_0000_0000_0000 -             User-space (lower half)
0x0000_003F_FFFF_FFFF               Currently unused; reserved for future use.

--- Sv39 canonical hole ---          Non-canonical addresses

0xFFFF_FFC0_0000_0000 -             Kernel space (upper half)
0xFFFF_FFFF_FFFF_FFFF
  0xFFFF_FFC0_8000_0000             Kernel direct map of RAM
  0xFFFF_FFC0_8020_0000             Kernel code (.text)
  0xFFFF_FFC0_0C00_0000             PLIC (mapped)
  0xFFFF_FFC0_1000_0000             UART (mapped)
  0xFFFF_FFC0_0200_0000             CLINT (mapped)
```

### Stack Layout (riscv64)

```
+---------------------------+ High address (SP initial value)
|       Guard Page          | 4 KB (unmapped)
+---------------------------+
|                           |
|     Kernel Stack          | 16 KB (4 pages)
|     (grows downward)      |
|                           |
+---------------------------+
|       Guard Page          | 4 KB (unmapped)
+---------------------------+ Low address
```

The RISC-V ABI requires 16-byte stack alignment. The `sp` register (x2) is set during
boot in `boot.S`.

---

## DMA Regions and Constraints

DMA (Direct Memory Access) allows devices to read and write system memory without CPU
involvement. Each architecture has specific constraints on DMA-accessible memory.

### x86-64 DMA Constraints

| DMA Type      | Address Range               | Size  | Notes                        |
|---------------|-----------------------------|-------|------------------------------|
| ISA DMA       | 0x0000_0000 - 0x00FF_FFFF   | 16 MB | Legacy 24-bit addressing     |
| PCI 32-bit    | 0x0000_0000 - 0xFFFF_FFFF   | 4 GB  | Standard PCI devices         |
| PCI 64-bit    | Full 64-bit range           | --    | PCIe with 64-bit BAR         |

- DMA buffers for legacy ISA devices must reside below 16 MB.
- PCI devices with 32-bit BARs can only DMA to addresses below 4 GB.
- All DMA buffers must be physically contiguous.
- Cache coherency is maintained by hardware (x86 is cache-coherent for DMA).

### ARM64 DMA Constraints

| DMA Type      | Address Range               | Notes                            |
|---------------|-----------------------------|----------------------------------|
| VirtIO        | Anywhere in RAM             | Uses virtqueue descriptors       |
| PCIe 32-bit   | 0x0000_0000 - 0xFFFF_FFFF  | Limited to lower 4 GB           |
| PCIe 64-bit   | Full range                  | Requires IOMMU or identity map  |

- ARM64 is **not** inherently cache-coherent for DMA. Buffers must be explicitly
  flushed/invalidated using cache maintenance operations (`DC CIVAC`, `DC IVAC`).
- The SMMU (System MMU / IOMMU) can remap DMA addresses if present.
- AlJefra OS currently uses identity-mapped DMA (physical == DMA address).

### RISC-V 64 DMA Constraints

| DMA Type      | Address Range               | Notes                            |
|---------------|-----------------------------|----------------------------------|
| VirtIO MMIO   | Anywhere in RAM             | Uses virtqueue descriptors       |
| PCIe          | 0x0000_0000 - 0xFFFF_FFFF  | 32-bit only on QEMU virt        |

- RISC-V does not guarantee cache coherency for DMA. Fence instructions (`fence`,
  `fence.i`) must be used before and after DMA transfers.
- DMA buffers should be allocated from non-cached or write-through mapped regions when
  possible.
- The RISC-V IOMMU specification is still evolving; AlJefra OS uses identity DMA mapping.

### DMA Buffer Allocation

```c
// Allocate a physically contiguous DMA buffer
// Returns both physical and virtual addresses
void *dma_alloc(size_t size, uint64_t *phys_addr);

// Free a DMA buffer
void dma_free(void *virt_addr, size_t size);

// Flush cache for a DMA buffer (required on ARM64 and RISC-V)
void dma_cache_flush(void *virt_addr, size_t size);

// Invalidate cache for a DMA buffer (before reading DMA data)
void dma_cache_invalidate(void *virt_addr, size_t size);
```

---

## Framebuffer Memory Mapping

The framebuffer provides direct pixel access for display output. The mapping depends on
the display device and architecture.

### VGA / QEMU Standard VGA (1234:1111)

| Architecture | Physical Address              | Default Resolution | Pixel Format |
|-------------|-------------------------------|--------------------|--------------|
| x86_64      | 0x000A_0000 (legacy) or PCIe BAR | 1024x768       | 32-bit BGRA  |
| aarch64     | PCIe MMIO BAR                 | 1024x768           | 32-bit BGRA  |
| riscv64     | PCIe MMIO BAR                 | 1024x768           | 32-bit BGRA  |

### Framebuffer Virtual Mapping

The framebuffer is mapped into kernel virtual address space as uncacheable (write-
combining on x86-64) to ensure writes are immediately visible on screen:

```c
// Map the framebuffer into virtual address space
void *fb_map(uint64_t phys_addr, size_t size);

// Framebuffer layout
// Each pixel is 4 bytes: [Blue] [Green] [Red] [Alpha/Reserved]
// Stride = width * 4 (bytes per row, may include padding)

struct framebuffer {
    void     *base;       // Virtual address of framebuffer start
    uint64_t  phys;       // Physical address
    uint32_t  width;      // Width in pixels
    uint32_t  height;     // Height in pixels
    uint32_t  stride;     // Bytes per row (pitch)
    uint32_t  bpp;        // Bits per pixel (32)
};
```

### Memory-Mapped I/O (MMIO) Caching Attributes

| Region         | x86-64 PAT Setting | ARM64 MAIR Setting    | RISC-V PTE Bits         |
|----------------|---------------------|-----------------------|-------------------------|
| Device MMIO    | Uncacheable (UC)    | Device-nGnRnE         | PTE.{R,W}, no cache    |
| Framebuffer    | Write-Combining     | Normal Non-Cacheable  | PTE.{R,W}, no cache    |
| Normal RAM     | Write-Back (WB)     | Normal Cacheable      | PTE.{R,W,X} cached     |

---

## Summary Table

| Property            | x86-64              | ARM64 (aarch64)     | RISC-V 64 (riscv64)  |
|---------------------|---------------------|---------------------|-----------------------|
| Page table levels   | 4 (PML4)           | 4 (4KB granule)     | 3 (Sv39) or 4 (Sv48) |
| Page size           | 4 KB                | 4 KB                | 4 KB                  |
| Large pages         | 2 MB, 1 GB         | 2 MB, 1 GB         | 2 MB, 1 GB           |
| VA bits             | 48                  | 48                  | 39 (Sv39) / 48 (Sv48)|
| RAM start           | 0x0010_0000 (1 MB) | 0x4000_0000 (1 GB) | 0x8000_0000 (2 GB)   |
| Kernel load address | 0x0010_0000         | 0x4008_0000         | 0x8020_0000           |
| UART                | COM1 (I/O port 0x3F8)| 0x0900_0000 (PL011)| 0x1000_0000 (NS16550)|
| Interrupt controller| APIC (0xFEE0_0000) | GICv2 (0x0800_0000) | PLIC (0x0C00_0000)   |
| Timer               | APIC timer / HPET  | ARM Generic Timer   | CLINT mtime           |
| Kernel stack size   | 16 KB               | 16 KB               | 16 KB                 |
| Cache coherent DMA  | Yes                 | No (flush required) | No (fence required)   |
