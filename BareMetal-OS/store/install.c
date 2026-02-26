/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Driver Installation Implementation */

#include "install.h"
#include "verify.h"
#include "package.h"
#include "../kernel/driver_loader.h"
#include "../hal/hal.h"

hal_status_t ajdrv_install(const void *data, uint64_t size, hal_device_t *dev)
{
    /* Step 1: Verify signature */
    hal_status_t rc = ajdrv_verify(data, size);
    if (rc != HAL_OK) {
        hal_console_puts("[install] Signature verification failed\n");
        return rc;
    }

    /* Step 2: Check architecture */
    const ajdrv_header_t *hdr = (const ajdrv_header_t *)data;
    if (hdr->arch != AJDRV_ARCH_ANY && hdr->arch != (uint32_t)hal_arch()) {
        hal_console_puts("[install] Architecture mismatch\n");
        return HAL_NOT_SUPPORTED;
    }

    /* Step 3: Load the driver binary */
    rc = driver_load_runtime(data, size, dev);
    if (rc != HAL_OK) {
        hal_console_puts("[install] Driver load failed\n");
        return rc;
    }

    const char *name = (const char *)data + hdr->name_offset;
    hal_console_printf("[install] Successfully installed '%s'\n", name);
    return HAL_OK;
}

hal_status_t ajdrv_install_from_storage(const char *filename, hal_device_t *dev)
{
    /* Read the file from storage via the active storage driver */
    const driver_ops_t *stor = driver_get_storage();
    if (!stor || !stor->read) {
        hal_console_puts("[install] No storage driver available\n");
        return HAL_NO_DEVICE;
    }

    /* Allocate a buffer for the driver package (max 256KB) */
    uint64_t phys;
    uint64_t max_size = 256 * 1024;
    void *buf = hal_dma_alloc(max_size, &phys);
    if (!buf)
        return HAL_NO_MEMORY;

    /* TODO: Implement BMFS file lookup by name
     * For now, this would need filesystem support to translate
     * filename → LBA offset and size.
     */

    hal_console_printf("[install] TODO: Load '%s' from storage\n", filename);

    hal_dma_free(buf, max_size);
    return HAL_NOT_SUPPORTED;
}
