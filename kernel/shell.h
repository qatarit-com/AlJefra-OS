/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Interactive Shell
 * Simple command-line interface for kernel interaction.
 */

#ifndef ALJEFRA_SHELL_H
#define ALJEFRA_SHELL_H

#include "../hal/hal.h"

/* Pass the device list from kernel_main (call before shell_run) */
void shell_set_devices(hal_device_t *devs, uint32_t count);

/* Start the interactive shell (blocking — runs until reboot) */
void shell_run(void);

#endif /* ALJEFRA_SHELL_H */
