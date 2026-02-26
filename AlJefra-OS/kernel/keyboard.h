/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Unified Keyboard Input API
 *
 * Provides a single keyboard interface that abstracts over:
 *   - PS/2 keyboard (x86-64 via port I/O)
 *   - USB HID keyboard (via xHCI + HID driver)
 *   - Device-tree keyboards (ARM64/RISC-V, future)
 *
 * Uses a circular buffer to decouple interrupt/poll producers
 * from consumer code (shell, AI agent, etc.).
 */

#ifndef ALJEFRA_KEYBOARD_H
#define ALJEFRA_KEYBOARD_H

#include "../hal/hal.h"

/* Key event passed through the unified buffer */
typedef struct {
    uint8_t scancode;       /* Raw scancode (source-specific) */
    char    ascii;          /* ASCII character (0 if non-printable) */
    bool    pressed;        /* true = key down, false = key up */
    uint8_t modifiers;      /* Active modifier bitmask at event time */
} keyboard_event_t;

/* Modifier bitmask flags */
#define KB_MOD_SHIFT    (1u << 0)
#define KB_MOD_CTRL     (1u << 1)
#define KB_MOD_ALT      (1u << 2)
#define KB_MOD_CAPSLOCK (1u << 3)

/* Circular key buffer capacity (must be power of 2) */
#define KB_BUF_SIZE     64
#define KB_BUF_MASK     (KB_BUF_SIZE - 1)

/* Initialize the unified keyboard subsystem.
 * Probes PS/2 on x86-64, checks for loaded USB HID driver.
 * Safe to call on all architectures (no-op if no keyboard found). */
hal_status_t keyboard_init(void);

/* Poll all active keyboard backends for new key events.
 * Call this periodically from the main loop or a timer callback.
 * Events are pushed into the internal circular buffer. */
void keyboard_poll(void);

/* Return 1 if at least one key event is buffered, 0 otherwise.
 * Non-blocking. */
int keyboard_available(void);

/* Retrieve the next keyboard event from the buffer.
 * Returns HAL_OK and fills *event, or HAL_ERROR if buffer is empty.
 * Non-blocking. */
hal_status_t keyboard_get_event(keyboard_event_t *event);

/* Blocking read of one ASCII character.
 * Polls internally (with hal_timer_delay_us between polls)
 * until a printable key-down event arrives.
 * Returns the ASCII character. */
char keyboard_getchar(void);

/* Return the number of buffered events (0..KB_BUF_SIZE-1) */
uint32_t keyboard_buffered(void);

#endif /* ALJEFRA_KEYBOARD_H */
