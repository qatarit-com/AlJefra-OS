/* SPDX-License-Identifier: MIT */
/* AlJefra OS — x86-64 Timer HAL Implementation
 * Wraps the kernel's HPET/KVM clock via b_system(TIMECOUNTER).
 *
 * CRITICAL: b_system(TIMECOUNTER, 0, 0) returns nanoseconds since boot,
 * NOT microseconds or milliseconds.  See MEMORY.md bug fixes.
 */

#include "../../hal/hal.h"

/* BareMetal kernel API */
extern uint64_t b_system(uint64_t function, uint64_t var1, uint64_t var2);

/* b_system function codes */
#define SYS_TIMECOUNTER    0x00
#define SYS_CALLBACK_TIMER 0x60
#define SYS_TSC            0x1F
#define SYS_DELAY          0x72

/* -------------------------------------------------------------------------- */
/* Timer callback state                                                       */
/* -------------------------------------------------------------------------- */

static bool timer_initialized = false;
static hal_timer_callback_t armed_callback = 0;
static void *armed_ctx = 0;
static uint64_t armed_deadline_ns = 0;

/* Timer ISR thunk: called by kernel when timer fires */
static void timer_isr_thunk(void)
{
    if (armed_callback && armed_deadline_ns > 0) {
        uint64_t now = b_system(SYS_TIMECOUNTER, 0, 0);
        if (now >= armed_deadline_ns) {
            hal_timer_callback_t cb = armed_callback;
            void *ctx = armed_ctx;
            /* Disarm before calling to allow re-arming from callback */
            armed_callback = 0;
            armed_deadline_ns = 0;
            cb(ctx);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* HAL Timer API                                                              */
/* -------------------------------------------------------------------------- */

hal_status_t hal_timer_init(void)
{
    /* The kernel already initializes HPET or KVM clock during boot.
     * We just verify that TIMECOUNTER returns a sane value. */
    uint64_t t1 = b_system(SYS_TIMECOUNTER, 0, 0);
    uint64_t t2 = b_system(SYS_TIMECOUNTER, 0, 0);

    /* Sanity: time must be monotonically non-decreasing */
    if (t2 < t1) {
        return HAL_ERROR;
    }

    timer_initialized = true;
    return HAL_OK;
}

uint64_t hal_timer_ns(void)
{
    return b_system(SYS_TIMECOUNTER, 0, 0);
}

void hal_timer_delay_us(uint64_t us)
{
    if (us == 0) return;

    uint64_t target_ns = us * 1000ULL;
    uint64_t start = b_system(SYS_TIMECOUNTER, 0, 0);

    /* Busy-wait loop.  Use a compiler barrier to prevent the optimizer
     * from hoisting the b_system call out of the loop. */
    for (;;) {
        uint64_t now = b_system(SYS_TIMECOUNTER, 0, 0);
        if ((now - start) >= target_ns)
            break;
        /* Hint to the CPU that we are in a spin-wait (PAUSE instruction).
         * Reduces power consumption and avoids memory-order violations. */
        __asm__ volatile ("pause");
    }
}

hal_status_t hal_timer_arm(uint64_t ns, hal_timer_callback_t cb, void *ctx)
{
    if (ns == 0) {
        /* Disarm */
        armed_callback = 0;
        armed_ctx = 0;
        armed_deadline_ns = 0;
        b_system(SYS_CALLBACK_TIMER, 0, 0);
        return HAL_OK;
    }

    uint64_t now = b_system(SYS_TIMECOUNTER, 0, 0);
    armed_deadline_ns = now + ns;
    armed_ctx = ctx;
    armed_callback = cb;

    /* Register our thunk as the timer callback.
     * The kernel will call it on each timer tick. */
    b_system(SYS_CALLBACK_TIMER, (uint64_t)(uintptr_t)timer_isr_thunk, 0);

    return HAL_OK;
}

uint64_t hal_timer_freq_hz(void)
{
    /* The kernel timer returns nanoseconds, so effective frequency
     * is 1 GHz (1 tick = 1 ns).  For TSC-based timing, the actual
     * TSC frequency varies by CPU but the kernel abstracts this away. */
    return 1000000000ULL;
}
