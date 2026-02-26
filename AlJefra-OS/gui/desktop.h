/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Desktop Application
 * Main desktop environment with:
 *   - Top bar (OS name, network status, clock, Qatar flag)
 *   - Left panel: File browser (BMFS file listing)
 *   - Right panel: AI chat (message display + text input)
 *
 * Entry point for the graphical user interface.
 */

#ifndef ALJEFRA_DESKTOP_H
#define ALJEFRA_DESKTOP_H

#include "widgets.h"

/* ======================================================================
 * Desktop layout constants
 * ====================================================================== */
#define DESKTOP_TOPBAR_H      32
#define DESKTOP_FILE_PANEL_W  300    /* Left panel width (responsive) */
#define DESKTOP_MIN_CHAT_W    400    /* Minimum chat panel width */

/* ======================================================================
 * File browser limits
 * ====================================================================== */
#define DESKTOP_MAX_FILES     256
#define DESKTOP_FILENAME_MAX  64

/* ======================================================================
 * AI chat limits
 * ====================================================================== */
#define DESKTOP_MAX_MESSAGES  256
#define DESKTOP_MSG_TEXT_MAX  512

/* ======================================================================
 * Desktop state (exposed so kernel_main can check if GUI is active)
 * ====================================================================== */
typedef struct {
    int  running;           /* Main loop is active */
    int  screen_w;          /* Screen width */
    int  screen_h;          /* Screen height */
    int  needs_redraw;      /* Full redraw needed */
    int  net_connected;     /* Network status flag */
    int  file_count;        /* Number of files loaded */
} desktop_state_t;

/* ======================================================================
 * Public API
 * ====================================================================== */

/* Initialize the desktop.
 * Call gui_init() before this.
 * Sets up all panels, widgets, and initial state.                        */
void desktop_init(void);

/* Run the desktop main event loop.
 * Processes keyboard, mouse, and timer events.
 * Does not return until the desktop is shut down (Ctrl+Q / logout).     */
void desktop_run(void);

/* Request a full desktop redraw. */
void desktop_request_redraw(void);

/* ── File browser ── */

/* Refresh the file list from BMFS.
 * Clears the list and re-scans the filesystem.                          */
void desktop_refresh_files(void);

/* Add a file entry to the browser (used internally or by fs driver).    */
void desktop_add_file(const char *name, int size_kb);

/* ── AI chat ── */

/* Send a message to the AI and display the response.
 * The user message is displayed immediately.
 * The AI response appears when available (or a placeholder).            */
void desktop_send_message(const char *text);

/* Display a system message in the chat (not from user or AI).           */
void desktop_system_message(const char *text);

/* ── Status bar updates ── */

/* Update the network connection status indicator. */
void desktop_set_net_status(int connected);

/* Update the clock display (called from timer or main loop). */
void desktop_update_clock(int hours, int minutes, int seconds);

/* Get a pointer to the desktop state. */
desktop_state_t *desktop_get_state(void);

#endif /* ALJEFRA_DESKTOP_H */
