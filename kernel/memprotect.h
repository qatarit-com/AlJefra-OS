/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Memory Protection Setup
 *
 * Configures page-level memory permissions to enforce:
 *   - Kernel code:  Read + Execute        (no write)
 *   - Kernel data:  Read + Write + No-Exec (W^X)
 *   - Kernel stack: Read + Write + No-Exec, with guard page
 *   - User space:   Separate page tables starting at USER_BASE
 *
 * Uses the HAL MMU interface for portable page table manipulation.
 * Architecture-specific bits (NX, PXN/UXN, PTE flags) are handled
 * internally per #ifdef.
 */

#ifndef ALJEFRA_KERNEL_MEMPROTECT_H
#define ALJEFRA_KERNEL_MEMPROTECT_H

#include <stdint.h>

/* ---- Memory region constants ---- */

/* Kernel image lives at 1 MB (x86-64 multiboot) or architecture-specific base.
 * These are logical constants used by memprotect — actual addresses come from
 * linker-provided symbols. */

/* Kernel text (code) region — set as RX */
#define MEMPROTECT_REGION_TEXT    0

/* Kernel read-only data region — set as R (no write, no exec) */
#define MEMPROTECT_REGION_RODATA  1

/* Kernel data + BSS region — set as RW+NX */
#define MEMPROTECT_REGION_DATA    2

/* Kernel stack region — set as RW+NX with bottom guard page */
#define MEMPROTECT_REGION_STACK   3

/* User-space base address.  All user mappings start here. */
#define USER_BASE   0x0000000000400000ULL   /* 4 MB (above kernel) */

/* Default kernel stack size (64 KB) */
#define KERNEL_STACK_SIZE   (64 * 1024)

/* ---- Public API ---- */

/* Initialize memory protection for the kernel image.
 * Sets up NX bit support, write-protect (CR0.WP on x86-64),
 * and applies permissions to all kernel regions.
 *
 * Must be called after hal_mmu_init() and before user code runs.
 * Returns 0 on success, -1 on error. */
int memprotect_init(void);

/* Mark a physical/virtual page range as non-executable.
 * Adds the NX / XN / PTE.X=0 bit to page table entries in the range.
 * `addr` and `size` must be page-aligned. */
int memprotect_set_nx(uint64_t addr, uint64_t size);

/* Mark a physical/virtual page range as read-only.
 * Clears the writable bit in page table entries.
 * `addr` and `size` must be page-aligned. */
int memprotect_set_ro(uint64_t addr, uint64_t size);

/* Set up a guard page at the given address.
 * The page is unmapped (any access causes a page fault).
 * Used at the bottom of kernel stacks to catch overflow. */
int memprotect_guard_page(uint64_t addr);

#endif /* ALJEFRA_KERNEL_MEMPROTECT_H */
