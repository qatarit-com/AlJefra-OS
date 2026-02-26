/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Persistent Kernel Logging
 *
 * Ring-buffer-based kernel log with automatic flush to disk.
 * Each entry carries a timestamp, severity level, and message text.
 *
 * Usage:
 *   klog_init();
 *   klog(KLOG_INFO, "Boot complete, %u devices found", dev_count);
 *   klog_flush();   // explicit flush (also auto-flushes at 75% full)
 */

#ifndef ALJEFRA_KERNEL_KLOG_H
#define ALJEFRA_KERNEL_KLOG_H

#include <stdint.h>

/* ---- Log severity levels ---- */

typedef enum {
    KLOG_DEBUG = 0,
    KLOG_INFO  = 1,
    KLOG_WARN  = 2,
    KLOG_ERROR = 3,
    KLOG_FATAL = 4,
} klog_level_t;

/* ---- Ring buffer sizing ---- */

#define KLOG_MAX_ENTRIES    128
#define KLOG_MSG_SIZE       256    /* Max message bytes per entry (including NUL) */
#define KLOG_FILE_NAME      "klog.dat"

/* ---- Public API ---- */

/* Initialize the kernel log ring buffer.  Optionally replays the
 * previous log from disk if the filesystem is available. */
void klog_init(void);

/* Log a message at the given severity level.
 * Supports a printf-like format string: %d, %u, %x, %p, %s, %%.
 * Messages at levels below the current minimum level are discarded. */
void klog(klog_level_t level, const char *fmt, ...);

/* Flush the ring buffer contents to "klog.dat" on disk.
 * Returns 0 on success, -1 if the filesystem is not available. */
int klog_flush(void);

/* Print all current ring buffer entries to the console
 * (both serial and framebuffer via hal_console). */
void klog_dump(void);

/* Set the minimum log level.  Messages below this level are silently
 * discarded.  Default is KLOG_DEBUG (everything is logged). */
void klog_set_level(klog_level_t min_level);

#endif /* ALJEFRA_KERNEL_KLOG_H */
