/* SPDX-License-Identifier: MIT */
/* AlJefra OS — x86-64 SMP HAL Implementation
 * Multi-core management via BareMetal kernel SMP syscalls.
 *
 * The kernel handles INIT/SIPI AP startup sequences internally.
 * User-space dispatches work to APs via b_system(SMP_SET, core, entry).
 */

#include "../../hal/hal.h"

/* BareMetal kernel API */
extern uint64_t b_system(uint64_t function, uint64_t var1, uint64_t var2);

/* b_system function codes */
#define SYS_SMP_ID        0x10
#define SYS_SMP_NUMCORES  0x11
#define SYS_SMP_SET       0x12
#define SYS_SMP_GET       0x13
#define SYS_SMP_LOCK      0x14
#define SYS_SMP_UNLOCK    0x15
#define SYS_SMP_BUSY      0x16

/* -------------------------------------------------------------------------- */
/* Core state tracking                                                        */
/* -------------------------------------------------------------------------- */

#define MAX_CORES 256

static hal_core_info_t core_info[MAX_CORES];
static uint32_t num_cores = 0;
static bool smp_initialized = false;

/* Per-core work function and argument (used by trampoline) */
static volatile hal_smp_work_fn core_work_fn[MAX_CORES];
static volatile void *core_work_arg[MAX_CORES];
static volatile uint64_t core_done[MAX_CORES]; /* Set to 1 when work completes */

/* AP trampoline: called by the kernel on the target core.
 * The kernel passes the core's APIC ID in a way that we need to
 * re-read it here.  We use the work_fn indexed by core_id. */
static void ap_trampoline(void)
{
    /* Read our core ID */
    uint64_t id = b_system(SYS_SMP_ID, 0, 0);
    if (id >= MAX_CORES) return;

    hal_smp_work_fn fn = core_work_fn[id];
    void *arg = (void *)core_work_arg[id];

    if (fn) {
        core_info[id].state = HAL_CORE_ONLINE;
        fn(arg);
    }

    core_info[id].state = HAL_CORE_HALTED;
    core_done[id] = 1;

    /* Signal completion via memory barrier */
    __asm__ volatile ("mfence" ::: "memory");
}

/* -------------------------------------------------------------------------- */
/* HAL SMP API                                                                */
/* -------------------------------------------------------------------------- */

hal_status_t hal_smp_init(void)
{
    num_cores = (uint32_t)b_system(SYS_SMP_NUMCORES, 0, 0);
    if (num_cores == 0)
        num_cores = 1;
    if (num_cores > MAX_CORES)
        num_cores = MAX_CORES;

    uint64_t bsp_id = b_system(SYS_SMP_ID, 0, 0);

    for (uint32_t i = 0; i < num_cores; i++) {
        core_info[i].core_id = i;
        core_info[i].state = (i == bsp_id) ? HAL_CORE_ONLINE : HAL_CORE_HALTED;
        core_info[i].stack_top = 0;
        core_work_fn[i] = 0;
        core_work_arg[i] = 0;
        core_done[i] = 0;
    }

    smp_initialized = true;
    return HAL_OK;
}

uint32_t hal_smp_core_count(void)
{
    if (!smp_initialized) {
        return (uint32_t)b_system(SYS_SMP_NUMCORES, 0, 0);
    }
    return num_cores;
}

uint32_t hal_smp_core_id(void)
{
    return (uint32_t)b_system(SYS_SMP_ID, 0, 0);
}

hal_status_t hal_smp_start_core(uint32_t core_id, hal_smp_work_fn fn, void *arg)
{
    if (core_id >= num_cores)
        return HAL_ERROR;

    /* Store work function and argument for the trampoline */
    core_work_fn[core_id] = fn;
    core_work_arg[core_id] = arg;
    core_done[core_id] = 0;

    /* Memory barrier to ensure stores are visible before core starts */
    __asm__ volatile ("mfence" ::: "memory");

    /* Dispatch to the core via kernel.
     * b_system(SMP_SET, core_id, entry_point):
     *   RAX = core_id, RDX = entry_point address */
    b_system(SYS_SMP_SET, (uint64_t)core_id,
             (uint64_t)(uintptr_t)ap_trampoline);

    return HAL_OK;
}

hal_status_t hal_smp_send_work(uint32_t core_id, hal_smp_work_fn fn, void *arg)
{
    /* Same as start_core for BareMetal — there is no distinction between
     * "first start" and "send work" since cores return to idle after each task. */
    return hal_smp_start_core(core_id, fn, arg);
}

hal_status_t hal_smp_wait_core(uint32_t core_id)
{
    if (core_id >= num_cores)
        return HAL_ERROR;

    /* Spin-wait for the core to signal completion */
    while (!core_done[core_id]) {
        __asm__ volatile ("pause" ::: "memory");
    }

    return HAL_OK;
}

/* -------------------------------------------------------------------------- */
/* Spinlock implementation — x86-64 LOCK XCHG / MOV                          */
/*                                                                            */
/* We use test-and-test-and-set (TTAS) for better performance:                */
/* first check the lock value without LOCK prefix (cache-local read),         */
/* then attempt the atomic exchange only if the lock appears free.            */
/* -------------------------------------------------------------------------- */

void hal_spin_lock(hal_spinlock_t *lock)
{
    for (;;) {
        /* Test (cache-local read) */
        while (*lock != 0) {
            __asm__ volatile ("pause" ::: "memory");
        }
        /* Test-and-set (atomic) */
        uint64_t old;
        __asm__ volatile (
            "lock xchgq %0, %1"
            : "=r"(old), "+m"(*lock)
            : "0"((uint64_t)1)
            : "memory"
        );
        if (old == 0)
            return; /* Lock acquired */
    }
}

void hal_spin_unlock(hal_spinlock_t *lock)
{
    /* A simple store with a compiler barrier is sufficient on x86-64.
     * The x86 memory model guarantees that stores are not reordered
     * with older stores (store-store ordering). */
    __asm__ volatile ("" ::: "memory");
    *lock = 0;
}

int hal_spin_trylock(hal_spinlock_t *lock)
{
    uint64_t old;
    __asm__ volatile (
        "lock xchgq %0, %1"
        : "=r"(old), "+m"(*lock)
        : "0"((uint64_t)1)
        : "memory"
    );
    return (old == 0) ? 1 : 0; /* 1 = success, 0 = already locked */
}
