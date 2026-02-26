/* SPDX-License-Identifier: MIT */
/* AlJefra OS — x86-64 Interrupt HAL Implementation
 * Wraps existing APIC/IO-APIC/IDT via b_system() callback mechanism.
 *
 * The BareMetal kernel owns the IDT and APIC.  User-space programs can
 * register callbacks for timer, network, and keyboard interrupts via
 * b_system(CALLBACK_*).  This HAL layer provides a thin abstraction
 * that maps HAL IRQ numbers to those callback slots.
 */

#include "../../hal/hal.h"

/* BareMetal kernel API */
extern uint64_t b_system(uint64_t function, uint64_t var1, uint64_t var2);

/* b_system callback codes */
#define SYS_CALLBACK_TIMER    0x60
#define SYS_CALLBACK_NETWORK  0x61
#define SYS_CALLBACK_KEYBOARD 0x62

/* -------------------------------------------------------------------------- */
/* HAL IRQ numbers mapped to BareMetal callbacks                              */
/*                                                                            */
/* HAL IRQ 0  = Timer                                                         */
/* HAL IRQ 1  = Keyboard                                                      */
/* HAL IRQ 11 = Network (common PCI IRQ line)                                 */
/* -------------------------------------------------------------------------- */

#define MAX_IRQ_HANDLERS  256
#define MAX_EXCEPTION_HANDLERS 32

/* Handler table */
typedef struct {
    hal_irq_handler_t handler;
    void             *ctx;
} irq_slot_t;

static irq_slot_t irq_handlers[MAX_IRQ_HANDLERS];
static hal_exception_handler_t exception_handlers[MAX_EXCEPTION_HANDLERS];
static bool irq_initialized = false;

/* -------------------------------------------------------------------------- */
/* Internal thunks: called by BareMetal kernel, dispatch to HAL handlers      */
/* -------------------------------------------------------------------------- */

/* These are called from kernel interrupt context with no arguments.
 * We route them to the appropriate HAL handler. */

static void timer_thunk(void)
{
    if (irq_handlers[0].handler)
        irq_handlers[0].handler(0, irq_handlers[0].ctx);
}

static void network_thunk(void)
{
    if (irq_handlers[11].handler)
        irq_handlers[11].handler(11, irq_handlers[11].ctx);
}

static void keyboard_thunk(void)
{
    if (irq_handlers[1].handler)
        irq_handlers[1].handler(1, irq_handlers[1].ctx);
}

/* -------------------------------------------------------------------------- */
/* HAL Interrupt API                                                          */
/* -------------------------------------------------------------------------- */

hal_status_t hal_irq_init(void)
{
    /* Zero all handler slots */
    for (int i = 0; i < MAX_IRQ_HANDLERS; i++) {
        irq_handlers[i].handler = 0;
        irq_handlers[i].ctx = 0;
    }
    for (int i = 0; i < MAX_EXCEPTION_HANDLERS; i++) {
        exception_handlers[i] = 0;
    }

    irq_initialized = true;
    return HAL_OK;
}

hal_status_t hal_irq_register(uint32_t irq, hal_irq_handler_t handler, void *ctx)
{
    if (irq >= MAX_IRQ_HANDLERS)
        return HAL_ERROR;

    irq_handlers[irq].handler = handler;
    irq_handlers[irq].ctx = ctx;

    /* Wire up to BareMetal kernel callbacks for known IRQ lines */
    switch (irq) {
    case 0:  /* Timer */
        b_system(SYS_CALLBACK_TIMER, (uint64_t)(uintptr_t)timer_thunk, 0);
        break;
    case 1:  /* Keyboard */
        b_system(SYS_CALLBACK_KEYBOARD, (uint64_t)(uintptr_t)keyboard_thunk, 0);
        break;
    case 11: /* Network */
        b_system(SYS_CALLBACK_NETWORK, (uint64_t)(uintptr_t)network_thunk, 0);
        break;
    default:
        /* Other IRQ lines are not directly wirable through b_system callbacks.
         * Hardware-specific drivers should use MMIO APIC/IO-APIC directly. */
        break;
    }

    return HAL_OK;
}

hal_status_t hal_irq_unregister(uint32_t irq)
{
    if (irq >= MAX_IRQ_HANDLERS)
        return HAL_ERROR;

    irq_handlers[irq].handler = 0;
    irq_handlers[irq].ctx = 0;

    /* Disable the kernel callback */
    switch (irq) {
    case 0:
        b_system(SYS_CALLBACK_TIMER, 0, 0);
        break;
    case 1:
        b_system(SYS_CALLBACK_KEYBOARD, 0, 0);
        break;
    case 11:
        b_system(SYS_CALLBACK_NETWORK, 0, 0);
        break;
    default:
        break;
    }

    return HAL_OK;
}

void hal_irq_enable(uint32_t irq)
{
    /* On BareMetal, IRQs are enabled by registering callbacks.
     * For direct APIC manipulation, we would need MMIO access to the
     * IO-APIC redirection table.  The kernel handles this internally. */
    (void)irq;
}

void hal_irq_disable(uint32_t irq)
{
    /* Unregister effectively disables the callback path */
    (void)irq;
}

void hal_irq_eoi(uint32_t irq)
{
    (void)irq;
    /* Write EOI to the local APIC.
     * The local APIC EOI register is at APIC_BASE + 0xB0.
     * On BareMetal, APIC base is typically at 0xFEE00000 (default x2APIC). */
    volatile uint32_t *apic_eoi = (volatile uint32_t *)0xFEE000B0ULL;
    *apic_eoi = 0;
}

hal_status_t hal_exception_register(uint32_t vector, hal_exception_handler_t handler)
{
    if (vector >= MAX_EXCEPTION_HANDLERS)
        return HAL_ERROR;

    exception_handlers[vector] = handler;

    /* Note: On BareMetal, the kernel owns the IDT.  User-space programs
     * cannot directly modify IDT entries.  This stores the handler for
     * use if the kernel dispatches exceptions to user callbacks (future). */
    return HAL_OK;
}

uint32_t hal_irq_max(void)
{
    /* IO-APIC typically supports 24 IRQ lines (0-23).
     * We report 255 as the maximum to allow for MSI/MSI-X. */
    return MAX_IRQ_HANDLERS - 1;
}
