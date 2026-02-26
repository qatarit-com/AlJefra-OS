/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Driver Loader Implementation
 *
 * Manages built-in and runtime-loaded drivers.
 * Built-in drivers are registered at init time.
 * Runtime drivers come from .ajdrv packages via the marketplace.
 */

#include "driver_loader.h"
#include "../hal/hal.h"

/* ── String helpers (no libc) ── */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void *memcpy_simple(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ── Built-in driver registry ── */

static const driver_ops_t *g_builtin_drivers[MAX_DRIVERS];
static uint32_t g_builtin_count;

/* Loaded (active) drivers */
static struct {
    const driver_ops_t *ops;
    int                 active;
} g_loaded[MAX_DRIVERS];
static uint32_t g_loaded_count;

void driver_register_builtin(const driver_ops_t *ops)
{
    if (g_builtin_count < MAX_DRIVERS) {
        g_builtin_drivers[g_builtin_count++] = ops;
    }
}

hal_status_t driver_load_builtin(const char *name, hal_device_t *dev)
{
    for (uint32_t i = 0; i < g_builtin_count; i++) {
        if (str_eq(g_builtin_drivers[i]->name, name)) {
            const driver_ops_t *ops = g_builtin_drivers[i];

            /* Initialize the driver with the device */
            hal_status_t rc = HAL_OK;
            if (ops->init) {
                rc = ops->init(dev);
            }

            if (rc != HAL_OK) {
                hal_console_printf("[driver] Failed to init '%s': %d\n", name, rc);
                return rc;
            }

            /* Add to loaded list */
            if (g_loaded_count < MAX_DRIVERS) {
                g_loaded[g_loaded_count].ops = ops;
                g_loaded[g_loaded_count].active = 1;
                g_loaded_count++;
            }

            hal_console_printf("[driver] Loaded '%s'\n", name);
            return HAL_OK;
        }
    }

    hal_console_printf("[driver] Built-in '%s' not found\n", name);
    return HAL_NO_DEVICE;
}

/* ── Runtime driver loading ── */

/*
 * .ajdrv package format:
 *   [0x00] magic: "AJDV" (4 bytes)
 *   [0x04] version: uint32_t
 *   [0x08] arch: uint32_t (0=x86_64, 1=aarch64, 2=riscv64)
 *   [0x0C] code_offset: uint32_t (offset to executable code)
 *   [0x10] code_size: uint32_t
 *   [0x14] name_offset: uint32_t (offset to name string)
 *   [0x18] name_size: uint32_t
 *   [0x1C] signature_offset: uint32_t (offset to Ed25519 signature)
 *   [0x20] entry_offset: uint32_t (offset within code to entry function)
 *   [0x24-0x40] reserved
 *   [name_offset..] driver name (null-terminated)
 *   [code_offset..] relocatable binary code
 *   [signature_offset..] 64-byte Ed25519 signature over header+name+code
 */

#define AJDRV_MAGIC  0x56444A41  /* "AJDV" little-endian */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t arch;
    uint32_t code_offset;
    uint32_t code_size;
    uint32_t name_offset;
    uint32_t name_size;
    uint32_t signature_offset;
    uint32_t entry_offset;
    uint8_t  reserved[28];
} ajdrv_header_t;

hal_status_t driver_load_runtime(const void *ajdrv_data, uint64_t size, hal_device_t *dev)
{
    if (size < sizeof(ajdrv_header_t))
        return HAL_ERROR;

    const ajdrv_header_t *hdr = (const ajdrv_header_t *)ajdrv_data;

    /* Validate magic */
    if (hdr->magic != AJDRV_MAGIC) {
        hal_console_puts("[driver] Invalid .ajdrv magic\n");
        return HAL_ERROR;
    }

    /* Check architecture */
    if (hdr->arch != (uint32_t)hal_arch()) {
        hal_console_puts("[driver] Architecture mismatch\n");
        return HAL_NOT_SUPPORTED;
    }

    /* Bounds check */
    if (hdr->code_offset + hdr->code_size > size ||
        hdr->name_offset + hdr->name_size > size) {
        hal_console_puts("[driver] Invalid offsets\n");
        return HAL_ERROR;
    }

    /* TODO: Verify Ed25519 signature via store/verify.c */

    /* Extract driver name */
    const char *name = (const char *)ajdrv_data + hdr->name_offset;

    /* Allocate memory for the driver code */
    uint64_t code_phys;
    void *code_buf = hal_dma_alloc(hdr->code_size, &code_phys);
    if (!code_buf) {
        hal_console_puts("[driver] Out of memory for driver code\n");
        return HAL_NO_MEMORY;
    }

    /* Copy code to executable memory */
    memcpy_simple(code_buf, (const uint8_t *)ajdrv_data + hdr->code_offset, hdr->code_size);

    /* The entry point is a function that returns a driver_ops_t* */
    typedef const driver_ops_t *(*driver_entry_fn)(void);
    driver_entry_fn entry = (driver_entry_fn)((uint8_t *)code_buf + hdr->entry_offset);

    /* Call the driver's entry point to get its ops table */
    const driver_ops_t *ops = entry();
    if (!ops) {
        hal_console_printf("[driver] '%s' entry returned NULL\n", name);
        hal_dma_free(code_buf, hdr->code_size);
        return HAL_ERROR;
    }

    /* Initialize driver with device */
    hal_status_t rc = HAL_OK;
    if (ops->init) {
        rc = ops->init(dev);
    }

    if (rc != HAL_OK) {
        hal_console_printf("[driver] Runtime '%s' init failed: %d\n", name, rc);
        hal_dma_free(code_buf, hdr->code_size);
        return rc;
    }

    /* Add to loaded list */
    if (g_loaded_count < MAX_DRIVERS) {
        g_loaded[g_loaded_count].ops = ops;
        g_loaded[g_loaded_count].active = 1;
        g_loaded_count++;
    }

    hal_console_printf("[driver] Loaded runtime driver '%s'\n", name);
    return HAL_OK;
}

/* ── Query functions ── */

const driver_ops_t *driver_find(driver_category_t cat)
{
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (g_loaded[i].active && g_loaded[i].ops->category == cat)
            return g_loaded[i].ops;
    }
    return NULL;
}

const driver_ops_t *driver_find_by_name(const char *name)
{
    for (uint32_t i = 0; i < g_loaded_count; i++) {
        if (g_loaded[i].active && str_eq(g_loaded[i].ops->name, name))
            return g_loaded[i].ops;
    }
    return NULL;
}

const driver_ops_t *driver_get_network(void)
{
    return driver_find(DRIVER_CAT_NETWORK);
}

const driver_ops_t *driver_get_storage(void)
{
    return driver_find(DRIVER_CAT_STORAGE);
}

const driver_ops_t *driver_get_input(void)
{
    return driver_find(DRIVER_CAT_INPUT);
}

uint32_t driver_list(const driver_ops_t **out, uint32_t max)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_loaded_count && count < max; i++) {
        if (g_loaded[i].active)
            out[count++] = g_loaded[i].ops;
    }
    return count;
}
