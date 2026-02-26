/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Desktop Application Implementation
 * ~1500 lines.  Main desktop with three panels:
 *   - Top bar: OS name, network status, clock, Qatar flag
 *   - Left panel: File browser (BMFS file listing)
 *   - Right panel: AI chat (message display + text input)
 *
 * All state is static.  No malloc.  Runs in kernel space.
 */

#include "desktop.h"

/* Panel title bar height (matches widgets.c internal constant) */
#define PANEL_TITLE_H  28

/* ======================================================================
 * Desktop state
 * ====================================================================== */

static desktop_state_t g_desktop;

/* ── Widget pointers (set during init) ── */
static widget_t *g_root_panel;       /* Full-screen root */
static widget_t *g_topbar;           /* Top bar panel */
static widget_t *g_file_panel;       /* Left file browser panel */
static widget_t *g_chat_panel;       /* Right chat panel */
static widget_t *g_file_list;        /* File list view widget */
static widget_t *g_chat_view;        /* Chat message view widget */
static widget_t *g_chat_input;       /* Chat text input widget */
static widget_t *g_chat_send_btn;    /* Send button */
static widget_t *g_lbl_os_name;      /* "AlJefra OS" label in topbar */
static widget_t *g_lbl_net_status;   /* Network status label */
static widget_t *g_lbl_clock;        /* Clock label */
static widget_t *g_btn_refresh;      /* Refresh files button */
static widget_t *g_lbl_file_info;    /* File info label (bottom of file panel) */

/* ── Clock state ── */
static int g_clock_hours;
static int g_clock_minutes;
static int g_clock_seconds;
static char g_clock_buf[16];         /* "HH:MM:SS" */

/* ── Layout dimensions (computed from screen size) ── */
static int g_file_panel_w;
static int g_chat_panel_w;
static int g_content_h;

/* ======================================================================
 * Qatar flag drawing (11x8 pixel mini flag in the top bar)
 * Maroon (#8D1B3D) left 40%, white right 60%, serrated edge
 * ====================================================================== */

#define FLAG_W  22
#define FLAG_H  14
#define FLAG_MAROON  0x8D1B3D

static void draw_qatar_flag(int x, int y)
{
    /* The Qatar flag has a maroon field on the left and a white field on
       the right, separated by a nine-point serrated (zigzag) line.
       We approximate this at our small pixel size. */

    for (int fy = 0; fy < FLAG_H; fy++) {
        for (int fx = 0; fx < FLAG_W; fx++) {
            /* Serrated boundary -- zigzag at about x=8 with amplitude 2 */
            int zigzag_x;
            int tooth = fy % 3;       /* 0,1,2 repeating */
            if (tooth == 0)
                zigzag_x = 7;
            else if (tooth == 1)
                zigzag_x = 9;
            else
                zigzag_x = 7;

            uint32_t color;
            if (fx < zigzag_x)
                color = GUI_WHITE;
            else
                color = FLAG_MAROON;

            gui_putpixel(x + fx, y + fy, color);
        }
    }
}

/* ======================================================================
 * Format helpers
 * ====================================================================== */

static void format_clock(char *buf, int h, int m, int s)
{
    buf[0] = '0' + (char)(h / 10);
    buf[1] = '0' + (char)(h % 10);
    buf[2] = ':';
    buf[3] = '0' + (char)(m / 10);
    buf[4] = '0' + (char)(m % 10);
    buf[5] = ':';
    buf[6] = '0' + (char)(s / 10);
    buf[7] = '0' + (char)(s % 10);
    buf[8] = '\0';
}

static void format_file_entry(char *buf, int bufsz, const char *name, int size_kb)
{
    /* "filename     123 KB" */
    int name_len = gui_strlen(name);
    int pos = 0;

    /* Copy name */
    for (int i = 0; i < name_len && pos < bufsz - 12; i++)
        buf[pos++] = name[i];

    /* Pad with spaces to column 24 or at least 2 spaces */
    int target = 24;
    if (pos >= target) target = pos + 2;
    while (pos < target && pos < bufsz - 10)
        buf[pos++] = ' ';

    /* Size */
    char size_str[12];
    gui_itoa(size_kb, size_str, 12);
    int slen = gui_strlen(size_str);
    for (int i = 0; i < slen && pos < bufsz - 4; i++)
        buf[pos++] = size_str[i];

    buf[pos++] = ' ';
    buf[pos++] = 'K';
    buf[pos++] = 'B';
    buf[pos] = '\0';
}

/* ======================================================================
 * Callbacks
 * ====================================================================== */

/* Called when the user submits text in the chat input */
static void on_chat_submit(const char *text)
{
    if (!text || text[0] == '\0') return;

    desktop_send_message(text);
    textinput_clear(g_chat_input);

    /* Re-focus the input so the user can keep typing */
    widget_set_focus(g_chat_input);
}

/* Called when the Send button is clicked */
static void on_send_click(void)
{
    const char *text = textinput_get_text(g_chat_input);
    if (text && text[0]) {
        desktop_send_message(text);
        textinput_clear(g_chat_input);
        widget_set_focus(g_chat_input);
    }
}

/* Called when the Refresh button is clicked */
static void on_refresh_click(void)
{
    desktop_refresh_files();
}

/* Called when a file is selected in the list */
static void on_file_select(int index, const char *item)
{
    (void)index;
    /* Update the info label at the bottom of the file panel */
    if (g_lbl_file_info && item)
        label_set_text(g_lbl_file_info, item);

    g_desktop.needs_redraw = 1;
}

/* ======================================================================
 * Desktop initialization
 * ====================================================================== */

void desktop_init(void)
{
    gui_screen_t *scr = gui_get_screen();
    int sw = scr->w;
    int sh = scr->h;

    g_desktop.running = 0;
    g_desktop.screen_w = sw;
    g_desktop.screen_h = sh;
    g_desktop.needs_redraw = 1;
    g_desktop.net_connected = 0;
    g_desktop.file_count = 0;

    g_clock_hours = 0;
    g_clock_minutes = 0;
    g_clock_seconds = 0;
    format_clock(g_clock_buf, 0, 0, 0);

    /* Reset widget pool */
    widget_pool_reset();

    /* ── Compute layout ── */
    g_file_panel_w = DESKTOP_FILE_PANEL_W;
    if (sw < 800)
        g_file_panel_w = sw / 3;  /* Responsive for small screens */

    g_chat_panel_w = sw - g_file_panel_w;
    if (g_chat_panel_w < DESKTOP_MIN_CHAT_W && sw >= DESKTOP_MIN_CHAT_W) {
        g_chat_panel_w = DESKTOP_MIN_CHAT_W;
        g_file_panel_w = sw - g_chat_panel_w;
    }

    g_content_h = sh - DESKTOP_TOPBAR_H;

    /* ── Create root panel (full screen, no title) ── */
    g_root_panel = widget_panel(0, 0, sw, sh, (const char *)0);
    if (!g_root_panel) return;
    g_root_panel->bg_color = GUI_BG;
    g_root_panel->border_color = GUI_BG;  /* No visible border */

    /* ================================================================
     * TOP BAR
     * ================================================================ */
    g_topbar = widget_panel(0, 0, sw, DESKTOP_TOPBAR_H, (const char *)0);
    if (!g_topbar) return;
    g_topbar->bg_color = GUI_BG2;
    g_topbar->border_color = GUI_BG2;
    panel_add_child(g_root_panel, g_topbar);

    /* OS name label (left side) */
    g_lbl_os_name = widget_label(10, 0, 200, DESKTOP_TOPBAR_H,
                                  "AlJefra OS v1.0");
    if (g_lbl_os_name) {
        g_lbl_os_name->fg_color = GUI_BLUE;
        panel_add_child(g_topbar, g_lbl_os_name);
    }

    /* Network status (center-left) */
    g_lbl_net_status = widget_label(220, 0, 160, DESKTOP_TOPBAR_H,
                                     "NET: Offline");
    if (g_lbl_net_status) {
        g_lbl_net_status->fg_color = GUI_TEXT2;
        label_set_color(g_lbl_net_status, GUI_TEXT2);
        panel_add_child(g_topbar, g_lbl_net_status);
    }

    /* Clock (right side) */
    g_lbl_clock = widget_label(sw - 100, 0, 80, DESKTOP_TOPBAR_H,
                                g_clock_buf);
    if (g_lbl_clock) {
        g_lbl_clock->d.label.align = ALIGN_RIGHT;
        g_lbl_clock->fg_color = GUI_TEXT;
        panel_add_child(g_topbar, g_lbl_clock);
    }

    /* Qatar flag is drawn manually in the draw phase (not a widget) */

    /* Separator line at bottom of top bar */
    /* (drawn as a 1px panel) */
    widget_t *topbar_sep = widget_panel(0, DESKTOP_TOPBAR_H - 1, sw, 1,
                                         (const char *)0);
    if (topbar_sep) {
        topbar_sep->bg_color = GUI_BORDER;
        topbar_sep->border_color = GUI_BORDER;
        panel_add_child(g_topbar, topbar_sep);
    }

    /* ================================================================
     * LEFT PANEL: File Browser
     * ================================================================ */
    g_file_panel = widget_panel(0, DESKTOP_TOPBAR_H, g_file_panel_w,
                                 g_content_h, "Files");
    if (!g_file_panel) return;
    g_file_panel->bg_color = GUI_BG2;
    g_file_panel->d.panel.title_bg = GUI_BG3;
    panel_add_child(g_root_panel, g_file_panel);

    /* Interior dimensions (inside panel border + title bar) */
    int fp_inner_w = g_file_panel_w - 2;
    int fp_inner_h = g_content_h - PANEL_TITLE_H - 2;

    /* Refresh button */
    g_btn_refresh = widget_button(8, 4, 100, 28, "Refresh",
                                   on_refresh_click);
    if (g_btn_refresh)
        panel_add_child(g_file_panel, g_btn_refresh);

    /* File list view */
    g_file_list = widget_listview(4, 38, fp_inner_w - 8,
                                   fp_inner_h - 74, on_file_select);
    if (g_file_list)
        panel_add_child(g_file_panel, g_file_list);

    /* File info label at bottom */
    g_lbl_file_info = widget_label(8, fp_inner_h - 30, fp_inner_w - 16,
                                    24, "Select a file");
    if (g_lbl_file_info) {
        g_lbl_file_info->fg_color = GUI_TEXT2;
        label_set_color(g_lbl_file_info, GUI_TEXT2);
        panel_add_child(g_file_panel, g_lbl_file_info);
    }

    /* ================================================================
     * RIGHT PANEL: AI Chat
     * ================================================================ */
    g_chat_panel = widget_panel(g_file_panel_w, DESKTOP_TOPBAR_H,
                                 g_chat_panel_w, g_content_h, "AI Assistant");
    if (!g_chat_panel) return;
    g_chat_panel->bg_color = GUI_BG2;
    g_chat_panel->d.panel.title_bg = GUI_BG3;
    panel_add_child(g_root_panel, g_chat_panel);

    int cp_inner_w = g_chat_panel_w - 2;
    int cp_inner_h = g_content_h - PANEL_TITLE_H - 2;

    /* Input height: text input + send button row */
    int input_row_h = 36;
    int send_btn_w = 80;

    /* Chat message view (fills most of the panel) */
    g_chat_view = widget_chatview(4, 4, cp_inner_w - 8,
                                   cp_inner_h - input_row_h - 16);
    if (g_chat_view)
        panel_add_child(g_chat_panel, g_chat_view);

    /* Text input (bottom of chat panel) */
    int input_y = cp_inner_h - input_row_h - 4;
    int input_w = cp_inner_w - send_btn_w - 20;

    g_chat_input = widget_textinput(4, input_y, input_w, input_row_h,
                                     "Type a message...", on_chat_submit);
    if (g_chat_input)
        panel_add_child(g_chat_panel, g_chat_input);

    /* Send button */
    g_chat_send_btn = widget_button(input_w + 12, input_y,
                                     send_btn_w, input_row_h,
                                     "Send", on_send_click);
    if (g_chat_send_btn) {
        g_chat_send_btn->bg_color = GUI_BLUE;
        g_chat_send_btn->fg_color = GUI_WHITE;
        g_chat_send_btn->border_color = GUI_BLUE;
        panel_add_child(g_chat_panel, g_chat_send_btn);
    }

    /* ── Populate initial content ── */

    /* Welcome message in chat */
    chatview_add(g_chat_view,
        "Welcome to AlJefra OS. I am your AI assistant. "
        "Type a message below to get started.", 0);

    chatview_add(g_chat_view,
        "You can ask me about the system, manage files, "
        "or request help with any task.", 0);

    /* Populate file list with some default entries */
    desktop_refresh_files();

    /* Focus the chat input by default */
    widget_set_focus(g_chat_input);
}

/* ======================================================================
 * Drawing
 * ====================================================================== */

static void desktop_draw(void)
{
    gui_screen_t *scr = gui_get_screen();

    /* Clear to background */
    gui_reset_clip();
    gui_draw_rect(0, 0, scr->w, scr->h, GUI_BG);

    /* Draw widget tree */
    widget_draw(g_root_panel);

    /* Draw Qatar flag in the top bar (manually, not a widget) */
    /* Position: to the left of the clock */
    int flag_x = scr->w - 100 - FLAG_W - 16;
    int flag_y = (DESKTOP_TOPBAR_H - FLAG_H) / 2;
    draw_qatar_flag(flag_x, flag_y);

    /* Draw vertical separator between file and chat panels */
    gui_draw_vline(g_file_panel_w, DESKTOP_TOPBAR_H, g_content_h,
                    GUI_BORDER);

    /* Draw mouse cursor on top of everything */
    gui_mouse_t *mouse = gui_get_mouse();
    if (mouse->visible) {
        gui_save_under_cursor(mouse->x, mouse->y);
        gui_draw_cursor(mouse->x, mouse->y);
    }

    /* Flip back buffer to screen */
    gui_flip();
}

/* ======================================================================
 * Input processing
 * ====================================================================== */

/* Convert PS/2 scancode set 2 extended keys to GUI key codes.
 * The PS/2 driver sets the 'extended' flag for E0-prefixed scancodes.
 * We map those scancodes to our GUI_KEY_xxx constants here. */
static int ps2_scancode_to_gui_key(int scancode, int extended)
{
    if (extended) {
        switch (scancode) {
        case 0x75: return GUI_KEY_UP;
        case 0x72: return GUI_KEY_DOWN;
        case 0x6B: return GUI_KEY_LEFT;
        case 0x74: return GUI_KEY_RIGHT;
        case 0x6C: return GUI_KEY_HOME;
        case 0x69: return GUI_KEY_END;
        case 0x7D: return GUI_KEY_PGUP;
        case 0x7A: return GUI_KEY_PGDN;
        case 0x71: return GUI_KEY_DELETE;
        default:   return 0;
        }
    }

    /* Non-extended special keys */
    switch (scancode) {
    case 0x76: return GUI_KEY_ESCAPE;
    case 0x0D: return GUI_KEY_TAB;
    default:   return 0;  /* Will use ASCII from the driver */
    }
}

/* Process a keyboard event from the PS/2 or USB HID driver.
 * ascii = 0 means non-printable; use scancode for special keys. */
static void desktop_process_key(int ascii, int scancode, int extended,
                                 int pressed)
{
    if (!pressed) return;  /* Only process key-down events */

    gui_event_t evt;
    evt.type = EVT_KEY_DOWN;
    evt.mx = 0;
    evt.my = 0;
    evt.mb = 0;

    /* Map to GUI key code */
    int gui_key = ps2_scancode_to_gui_key(scancode, extended);
    if (gui_key) {
        evt.key = gui_key;
    } else if (ascii) {
        evt.key = ascii;
    } else {
        return;  /* Unknown key */
    }

    /* Global shortcuts */
    /* Ctrl+Q = quit desktop */
    if (evt.key == 17) {  /* Ctrl+Q = ASCII 17 */
        g_desktop.running = 0;
        return;
    }

    /* Tab: cycle focus between chat input and file list */
    if (evt.key == '\t' || evt.key == GUI_KEY_TAB) {
        widget_t *cur = widget_get_focus();
        if (cur == g_chat_input)
            widget_set_focus(g_file_list);
        else
            widget_set_focus(g_chat_input);
        g_desktop.needs_redraw = 1;
        return;
    }

    /* Dispatch to focused widget */
    widget_t *focused = widget_get_focus();
    if (focused) {
        if (widget_key_event(focused, &evt))
            g_desktop.needs_redraw = 1;
    }
}

/* Process a mouse movement or click */
static void desktop_process_mouse(int dx, int dy, int buttons)
{
    gui_mouse_t *mouse = gui_get_mouse();

    /* Restore the area under the old cursor position */
    gui_restore_under_cursor();

    /* Update position */
    mouse->x += dx;
    mouse->y += dy;

    /* Clamp to screen bounds */
    if (mouse->x < 0) mouse->x = 0;
    if (mouse->y < 0) mouse->y = 0;
    if (mouse->x >= g_desktop.screen_w)
        mouse->x = g_desktop.screen_w - 1;
    if (mouse->y >= g_desktop.screen_h)
        mouse->y = g_desktop.screen_h - 1;

    /* Detect button state changes */
    int prev = mouse->prev_buttons;
    mouse->prev_buttons = mouse->buttons;
    mouse->buttons = buttons;

    gui_event_t evt;
    evt.mx = mouse->x;
    evt.my = mouse->y;
    evt.mb = buttons;

    /* Mouse move event */
    if (dx != 0 || dy != 0) {
        evt.type = EVT_MOUSE_MOVE;
        widget_mouse_event(g_root_panel, &evt);
        g_desktop.needs_redraw = 1;
    }

    /* Button press (was 0, now 1) */
    int left_press = (buttons & 1) && !(prev & 1);
    int left_release = !(buttons & 1) && (prev & 1);

    if (left_press) {
        evt.type = EVT_MOUSE_DOWN;
        widget_mouse_event(g_root_panel, &evt);
        g_desktop.needs_redraw = 1;
    }

    if (left_release) {
        evt.type = EVT_MOUSE_UP;
        widget_mouse_event(g_root_panel, &evt);
        g_desktop.needs_redraw = 1;
    }
}

/* ======================================================================
 * Main event loop
 * ====================================================================== */

/* Note: In a real OS, this would call ps2_poll() and usb_hid_poll()
 * to get input events.  The extern declarations below allow linking
 * against the real drivers.  If running in a test harness, these can
 * be stubbed out. */

/* Extern driver state (defined elsewhere in the kernel) */
/* These are weak symbols -- if not linked, the loop just skips input */

/* PS/2 keyboard polling interface */
extern bool ps2_has_key(void *dev) __attribute__((weak));
extern bool ps2_get_key(void *dev, void *event) __attribute__((weak));
extern void ps2_poll(void *dev) __attribute__((weak));

/* USB HID mouse polling interface */
extern bool usb_hid_get_mouse(void *dev, void *event) __attribute__((weak));
extern int  usb_hid_poll(void *dev) __attribute__((weak));

/* Global device pointers (set by kernel init) */
extern void *g_ps2_dev  __attribute__((weak));
extern void *g_mouse_dev __attribute__((weak));

/* Simple spin delay (architecture-independent busy wait) */
static void desktop_delay(void)
{
    for (volatile int i = 0; i < 100000; i++)
        ;
}

void desktop_run(void)
{
    g_desktop.running = 1;
    g_desktop.needs_redraw = 1;

    /* Initial draw */
    desktop_draw();

    /* ── Main loop ── */
    while (g_desktop.running) {

        /* ── Poll PS/2 keyboard ── */
        if (ps2_poll && ps2_has_key && ps2_get_key && &g_ps2_dev) {
            ps2_poll(&g_ps2_dev);
            /* Read all pending key events */
            /* We use a generic struct layout matching ps2_key_event_t:
             *   uint8_t scancode; char ascii; bool pressed; bool extended; */
            struct { uint8_t scancode; char ascii; uint8_t pressed; uint8_t extended; } kev;
            while (ps2_get_key(&g_ps2_dev, &kev)) {
                desktop_process_key(kev.ascii, kev.scancode,
                                     kev.extended, kev.pressed);
            }
        }

        /* ── Poll USB HID mouse ── */
        if (usb_hid_poll && usb_hid_get_mouse && &g_mouse_dev) {
            usb_hid_poll(&g_mouse_dev);
            struct { int16_t dx; int16_t dy; int8_t wheel; uint8_t buttons; } mev;
            while (usb_hid_get_mouse(&g_mouse_dev, &mev)) {
                desktop_process_mouse(mev.dx, mev.dy, mev.buttons);
            }
        }

        /* ── Process queued events ── */
        gui_event_t evt;
        while (gui_poll_event(&evt)) {
            switch (evt.type) {
            case EVT_KEY_DOWN:
                desktop_process_key(evt.key, 0, 0, 1);
                break;
            case EVT_MOUSE_MOVE:
            case EVT_MOUSE_DOWN:
            case EVT_MOUSE_UP:
            case EVT_MOUSE_CLICK:
                desktop_process_mouse(0, 0, evt.mb);
                break;
            case EVT_REDRAW:
                g_desktop.needs_redraw = 1;
                break;
            default:
                break;
            }
        }

        /* ── Redraw if needed ── */
        if (g_desktop.needs_redraw) {
            desktop_draw();
            g_desktop.needs_redraw = 0;
        }

        /* ── Small delay to avoid 100% CPU ── */
        desktop_delay();
    }
}

/* ======================================================================
 * File browser
 * ====================================================================== */

void desktop_refresh_files(void)
{
    if (!g_file_list) return;

    listview_clear(g_file_list);
    g_desktop.file_count = 0;

    /* In a real OS, this would read from BMFS via b_nvs_read.
     * For now, populate with the known system files. */
    desktop_add_file("kernel.bin", 147);
    desktop_add_file("aljefra.cfg", 1);
    desktop_add_file("ai_agent.app", 251);
    desktop_add_file("hello.app", 2);
    desktop_add_file("ethtest.app", 4);
    desktop_add_file("smptest.app", 3);
    desktop_add_file("readme.txt", 1);

    if (g_lbl_file_info) {
        char info[64];
        gui_itoa(g_desktop.file_count, info, 64);
        int len = gui_strlen(info);
        const char *suffix = " files";
        int slen = gui_strlen(suffix);
        for (int i = 0; i < slen && len + i < 63; i++)
            info[len + i] = suffix[i];
        info[len + slen] = '\0';
        label_set_text(g_lbl_file_info, info);
    }

    g_desktop.needs_redraw = 1;
}

void desktop_add_file(const char *name, int size_kb)
{
    if (!g_file_list || !name) return;

    char entry[WIDGET_LIST_ITEM_MAX];
    format_file_entry(entry, WIDGET_LIST_ITEM_MAX, name, size_kb);
    listview_add(g_file_list, entry);
    g_desktop.file_count++;
}

/* ======================================================================
 * AI Chat
 * ====================================================================== */

void desktop_send_message(const char *text)
{
    if (!g_chat_view || !text || text[0] == '\0') return;

    /* Display user message */
    chatview_add(g_chat_view, text, 1);

    /* In a real OS, this would call the AI agent (HTTP POST to Claude API).
     * For now, generate a local response based on the input. */

    /* Simple keyword-based responses for offline mode */
    const char *response = (const char *)0;

    /* Check for some keywords */
    int len = gui_strlen(text);

    /* "hello" / "hi" */
    if ((len >= 2 && text[0] == 'h' && text[1] == 'i') ||
        (len >= 5 && text[0] == 'h' && text[1] == 'e' &&
         text[2] == 'l' && text[3] == 'l' && text[4] == 'o')) {
        response = "Hello! How can I help you today?";
    }
    /* "help" */
    else if (len >= 4 && text[0] == 'h' && text[1] == 'e' &&
             text[2] == 'l' && text[3] == 'p') {
        response = "Available commands:\n"
                   "- 'files' : List files on disk\n"
                   "- 'status' : Show system status\n"
                   "- 'arch' : Show architecture info\n"
                   "- 'net' : Show network status\n"
                   "- 'clear' : Clear chat history";
    }
    /* "files" */
    else if (len >= 5 && text[0] == 'f' && text[1] == 'i' &&
             text[2] == 'l' && text[3] == 'e' && text[4] == 's') {
        desktop_refresh_files();
        response = "File list refreshed. Check the Files panel on the left.";
    }
    /* "status" */
    else if (len >= 6 && text[0] == 's' && text[1] == 't' &&
             text[2] == 'a' && text[3] == 't' && text[4] == 'u' &&
             text[5] == 's') {
        response = "AlJefra OS v1.0 running.\n"
                   "Architecture: Universal (x86-64/ARM64/RISC-V)\n"
                   "GUI: Active\n"
                   "Widgets: Panel, Label, Button, TextInput, "
                   "ListView, ChatView";
    }
    /* "arch" */
    else if (len >= 4 && text[0] == 'a' && text[1] == 'r' &&
             text[2] == 'c' && text[3] == 'h') {
        response = "Supported architectures:\n"
                   "- x86-64 (AMD64/Intel 64)\n"
                   "- AArch64 (ARM64 Cortex-A72+)\n"
                   "- RISC-V 64 (RV64GC, Sv39 MMU)\n"
                   "All three boot to desktop via HAL abstraction.";
    }
    /* "net" */
    else if (len >= 3 && text[0] == 'n' && text[1] == 'e' &&
             text[2] == 't') {
        if (g_desktop.net_connected)
            response = "Network: Connected\n"
                       "Drivers: e1000, VirtIO-Net, RTL8169\n"
                       "Stack: TCP/IP + TLS 1.2 (BearSSL)";
        else
            response = "Network: Offline\n"
                       "Connect a network adapter to enable "
                       "AI cloud features.";
    }
    /* "clear" */
    else if (len >= 5 && text[0] == 'c' && text[1] == 'l' &&
             text[2] == 'e' && text[3] == 'a' && text[4] == 'r') {
        chatview_clear(g_chat_view);
        desktop_system_message("Chat history cleared.");
        g_desktop.needs_redraw = 1;
        return;
    }
    /* Default response */
    else {
        response = "I received your message. In online mode, "
                   "I would connect to the Claude API for a full response. "
                   "Type 'help' for available offline commands.";
    }

    if (response)
        chatview_add(g_chat_view, response, 0);

    g_desktop.needs_redraw = 1;
}

void desktop_system_message(const char *text)
{
    if (!g_chat_view || !text) return;

    /* System messages appear as AI messages with a prefix */
    char sys_msg[WIDGET_CHAT_TEXT_MAX];
    gui_strncpy(sys_msg, "[System] ", WIDGET_CHAT_TEXT_MAX);
    int prefix_len = gui_strlen(sys_msg);
    int text_len = gui_strlen(text);
    int copy_len = text_len;
    if (prefix_len + copy_len >= WIDGET_CHAT_TEXT_MAX)
        copy_len = WIDGET_CHAT_TEXT_MAX - prefix_len - 1;
    for (int i = 0; i < copy_len; i++)
        sys_msg[prefix_len + i] = text[i];
    sys_msg[prefix_len + copy_len] = '\0';

    chatview_add(g_chat_view, sys_msg, 0);
    g_desktop.needs_redraw = 1;
}

/* ======================================================================
 * Status bar updates
 * ====================================================================== */

void desktop_set_net_status(int connected)
{
    g_desktop.net_connected = connected;

    if (g_lbl_net_status) {
        if (connected) {
            label_set_text(g_lbl_net_status, "NET: Online");
            label_set_color(g_lbl_net_status, GUI_GREEN);
        } else {
            label_set_text(g_lbl_net_status, "NET: Offline");
            label_set_color(g_lbl_net_status, GUI_TEXT2);
        }
    }

    g_desktop.needs_redraw = 1;
}

void desktop_update_clock(int hours, int minutes, int seconds)
{
    g_clock_hours = hours;
    g_clock_minutes = minutes;
    g_clock_seconds = seconds;

    format_clock(g_clock_buf, hours, minutes, seconds);

    if (g_lbl_clock)
        label_set_text(g_lbl_clock, g_clock_buf);

    g_desktop.needs_redraw = 1;
}

void desktop_request_redraw(void)
{
    g_desktop.needs_redraw = 1;
}

desktop_state_t *desktop_get_state(void)
{
    return &g_desktop;
}
