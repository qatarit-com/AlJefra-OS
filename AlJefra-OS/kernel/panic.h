/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Kernel Panic and Crash Recovery
 *
 * Provides a structured panic mechanism with register dump, stack trace,
 * crash log persistence, and configurable auto-reboot.  Panic output is
 * sent to both the serial console and the framebuffer (red screen).
 *
 * Usage:
 *   kernel_panic("out of memory");
 *   kernel_assert(ptr != NULL, "null pointer in scheduler");
 */

#ifndef ALJEFRA_KERNEL_PANIC_H
#define ALJEFRA_KERNEL_PANIC_H

#include <stdint.h>

/* Maximum depth for stack trace frames */
#define PANIC_MAX_FRAMES  16

/* Maximum length of the reason string stored in panic_info_t */
#define PANIC_REASON_MAX  128

/* ---- Saved register set (architecture-neutral superset) ---- */

typedef struct {
    uint64_t regs[32];      /* General-purpose registers (by index) */
    uint64_t pc;            /* Program counter / RIP / ELR_EL1 */
    uint64_t sp;            /* Stack pointer */
    uint64_t flags;         /* RFLAGS / SPSR_EL1 / sstatus */
} panic_regs_t;

/* ---- Panic information block ---- */

typedef struct {
    char         reason[PANIC_REASON_MAX]; /* Human-readable reason string */
    panic_regs_t regs;                     /* Register snapshot */
    uint64_t     backtrace[PANIC_MAX_FRAMES]; /* Return-address stack trace */
    uint32_t     backtrace_depth;          /* Number of valid backtrace entries */
    uint64_t     timestamp_ns;             /* hal_timer_ns() at panic time */
    uint32_t     cpu_id;                   /* Core that panicked */
} panic_info_t;

/* Panic handler callback — invoked before the default reboot/halt action */
typedef void (*panic_handler_t)(const panic_info_t *info);

/* ---- Public API ---- */

/* Trigger a kernel panic.  Does not return. */
void kernel_panic(const char *reason) __attribute__((noreturn));

/* Register a custom panic handler (called before auto-reboot).
 * Only one handler may be registered; a second call replaces the first. */
void panic_register_handler(panic_handler_t handler);

/* Set the auto-reboot countdown in seconds (0 = halt, no reboot).
 * Default is 10 seconds. */
void panic_set_reboot_timeout(uint32_t seconds);

/* Assert macro — panics with file:line and message if condition is false */
#define kernel_assert(cond, msg)                                        \
    do {                                                                \
        if (!(cond))                                                    \
            kernel_panic("ASSERT FAIL: " msg " [" __FILE__ "]");       \
    } while (0)

#endif /* ALJEFRA_KERNEL_PANIC_H */
