/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Driver Installation */

#ifndef ALJEFRA_INSTALL_H
#define ALJEFRA_INSTALL_H

#include <stdint.h>
#include "../hal/hal.h"

/* Install a driver from an .ajdrv package buffer.
 * Verifies signature, checks architecture, loads the driver.
 * dev can be NULL if the device is not yet known.
 */
hal_status_t ajdrv_install(const void *data, uint64_t size, hal_device_t *dev);

/* Install a driver from storage (by filename in BMFS) */
hal_status_t ajdrv_install_from_storage(const char *filename, hal_device_t *dev);

#endif /* ALJEFRA_INSTALL_H */
