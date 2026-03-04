/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Widget Toolkit Implementation
 *
 * ~1500 lines.  All widget drawing, event handling, focus management,
 * and static pool allocation.  Runs bare-metal -- no malloc anywhere.
 *
 * Drawing uses gui.h primitives: gui_draw_rect, gui_draw_text,
 * gui_draw_rect_border, gui_draw_text_wrap, gui_push_clip, gui_pop_clip.
 */

#include "widgets.h"

/* ======================================================================
 * Static widget pool
 * ====================================================================== */

static widget_t  g_widget_pool[WIDGET_MAX_TOTAL];
static int       g_widget_count;

/* Currently focused widget */
static widget_t *g_focused_widget;

/* ======================================================================
 * Widget pool management
 * ====================================================================== */

static widget_t *widget_alloc(void)
{
    if (g_widget_count >= WIDGET_MAX_TOTAL)
        return (widget_t *)0;

    widget_t *w = &g_widget_pool[g_widget_count++];

    /* Zero-initialize */
    char *p = (char *)w;
    for (int i = 0; i < (int)sizeof(widget_t); i++)
        p[i] = 0;

    w->visible = 1;
    w->enabled = 1;
    w->bg_color = GUI_BG2;
    w->fg_color = GUI_TEXT;
    w->border_color = GUI_BORDER;

    return w;
}

void widget_pool_reset(void)
{
    g_widget_count = 0;
    g_focused_widget = (widget_t *)0;
}

int widget_pool_count(void)
{
    return g_widget_count;
}

/* ======================================================================
 * Focus management
 * ====================================================================== */

void widget_set_focus(widget_t *w)
{
    if (g_focused_widget)
        g_focused_widget->focused = 0;

    g_focused_widget = w;

    if (w) {
        w->focused = 1;
        if (w->type == W_TEXTINPUT) {
            w->data.input.cursor_timer = 0;
            w->data.input.cursor_visible = 1;
        }
    }
}

widget_t *widget_get_focus(void)
{
    return g_focused_widget;
}

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

static void widget_compute_abs(widget_t *w)
{
    w->abs_x = w->bounds.x;
    w->abs_y = w->bounds.y;

    widget_t *p = w->parent;
    while (p) {
        w->abs_x += p->abs_x;
        w->abs_y += p->abs_y;

        if (p->type == W_PANEL && p->data.panel.show_title)
            w->abs_y += 28;

        p = p->parent;
    }
}

#define PANEL_TITLE_H    28
#define LIST_ITEM_H      24
#define CHAT_PAD_X       10
#define CHAT_PAD_Y       6
#define CHAT_MSG_GAP     8
#define SCROLLBAR_W      10
#define INPUT_PAD_X      6
#define CURSOR_BLINK_RATE 25

/* ======================================================================
 * Terminal static buffers (single instance — no malloc)
 * ====================================================================== */

static char    g_term_cells[TERM_BUF_ROWS][TERM_COLS_MAX];
static uint8_t g_term_attrs[TERM_BUF_ROWS][TERM_COLS_MAX];
static char    g_term_input[TERM_INPUT_MAX];
static char    g_term_input_save[TERM_INPUT_MAX];
static int     g_term_input_save_len;
static char    g_term_history[TERM_HISTORY_MAX][TERM_INPUT_MAX];
static char    g_term_prompt[TERM_PROMPT_MAX] = "$ ";
static int     g_term_prompt_len = 2;

/* ANSI color palette (16 colors, matching AlJefra dark theme) */
static const uint32_t g_ansi_colors[16] = {
    0x0D1117,  /*  0: Black       */
    0xF85149,  /*  1: Red         */
    0x3FB950,  /*  2: Green       */
    0xD29922,  /*  3: Yellow      */
    0x58A6FF,  /*  4: Blue        */
    0xBC8CFF,  /*  5: Magenta     */
    0x39D2E0,  /*  6: Cyan        */
    0xC9D1D9,  /*  7: White       */
    0x484F58,  /*  8: Bright Blk  */
    0xFF7B72,  /*  9: Bright Red  */
    0x56D364,  /* 10: Bright Grn  */
    0xE3B341,  /* 11: Bright Yel  */
    0x79C0FF,  /* 12: Bright Blu  */
    0xD2A8FF,  /* 13: Bright Mag  */
    0x56D4E0,  /* 14: Bright Cyn  */
    0xFFFFFF,  /* 15: Bright Wht  */
};

#define TERM_DEFAULT_ATTR  0x07  /* White on black  */
#define TERM_PROMPT_ATTR   0x02  /* Green on black  */

/* ANSI escape parser states */
#define ESC_NONE     0
#define ESC_GOT_ESC  1
#define ESC_CSI      2

/* ======================================================================
 * Panel drawing
 * ====================================================================== */

static void draw_panel(widget_t *w)
{
    panel_data_t *pd = &w->data.panel;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    gui_draw_rect_border(x, y, bw, bh, w->border_color, w->bg_color);

    if (pd->show_title) {
        uint32_t tbg = pd->title_bg ? pd->title_bg : GUI_BG3;
        gui_draw_rect(x + 1, y + 1, bw - 2, PANEL_TITLE_H - 1, tbg);
        gui_draw_hline(x + 1, y + PANEL_TITLE_H, bw - 2, w->border_color);

        int tx = x + 10;
        int ty = y + (PANEL_TITLE_H - GUI_FONT_H) / 2;
        gui_draw_text(tx, ty, pd->title, GUI_TEXT);
    }

    int interior_y = y + (pd->show_title ? PANEL_TITLE_H + 1 : 1);
    int interior_h = bh - (pd->show_title ? PANEL_TITLE_H + 2 : 2);
    gui_rect_t clip = { x + 1, interior_y, bw - 2, interior_h };
    gui_push_clip(&clip);

    for (int i = 0; i < pd->count; i++) {
        if (pd->children[i] && pd->children[i]->visible)
            widget_draw(pd->children[i]);
    }

    gui_pop_clip();
}

/* ======================================================================
 * Label drawing
 * ====================================================================== */

static void draw_label(widget_t *w)
{
    label_data_t *ld = &w->data.label;
    int x = w->abs_x;
    int y = w->abs_y;

    if (ld->wrap) {
        gui_draw_text_wrap(x, y, w->bounds.w, ld->text, w->fg_color);
    } else {
        int text_len = gui_strlen(ld->text);
        int text_w = text_len * GUI_FONT_W;
        int tx = x;

        if (ld->align == ALIGN_CENTER)
            tx = x + (w->bounds.w - text_w) / 2;
        else if (ld->align == ALIGN_RIGHT)
            tx = x + w->bounds.w - text_w;

        int ty = y + (w->bounds.h - GUI_FONT_H) / 2;
        gui_draw_text(tx, ty, ld->text, w->fg_color);
    }
}

/* ======================================================================
 * Button drawing
 * ====================================================================== */

static void draw_button(widget_t *w)
{
    button_data_t *bd = &w->data.button;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    uint32_t bg, fg, border;

    if (!w->enabled) {
        bg = GUI_BG3;
        fg = GUI_TEXT2;
        border = GUI_BORDER;
    } else if (bd->pressed) {
        bg = GUI_BLUE;
        fg = GUI_WHITE;
        border = GUI_BLUE;
    } else if (bd->hover) {
        bg = GUI_BG3;
        fg = GUI_WHITE;
        border = GUI_BLUE;
    } else {
        bg = GUI_BG3;
        fg = w->fg_color;
        border = w->border_color;
    }

    gui_draw_rect_border(x, y, bw, bh, border, bg);

    int text_len = gui_strlen(bd->text);
    int text_w = text_len * GUI_FONT_W;
    int tx = x + (bw - text_w) / 2;
    int ty = y + (bh - GUI_FONT_H) / 2;
    gui_draw_text(tx, ty, bd->text, fg);
}

/* ======================================================================
 * Text Input drawing
 * ====================================================================== */

static void draw_textinput(widget_t *w)
{
    textinput_data_t *td = &w->data.input;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    uint32_t border = w->focused ? GUI_BLUE : w->border_color;
    gui_draw_rect_border(x, y, bw, bh, border, GUI_BG);

    gui_rect_t clip = { x + INPUT_PAD_X, y + 1,
                        bw - INPUT_PAD_X * 2, bh - 2 };
    gui_push_clip(&clip);

    int text_y = y + (bh - GUI_FONT_H) / 2;

    if (td->len == 0 && !w->focused) {
        gui_draw_text(x + INPUT_PAD_X, text_y,
                      td->placeholder, GUI_TEXT2);
    } else {
        int visible_chars = (bw - INPUT_PAD_X * 2) / GUI_FONT_W;
        if (visible_chars <= 0) visible_chars = 1;

        if (td->cursor < td->scroll)
            td->scroll = td->cursor;
        if (td->cursor >= td->scroll + visible_chars)
            td->scroll = td->cursor - visible_chars + 1;
        if (td->scroll < 0)
            td->scroll = 0;

        int draw_x = x + INPUT_PAD_X;
        for (int i = td->scroll; i < td->len && i < td->scroll + visible_chars; i++) {
            gui_draw_char_transparent(draw_x, text_y,
                                       td->text[i], w->fg_color);
            draw_x += GUI_FONT_W;
        }

        /* Blinking cursor when focused */
        if (w->focused) {
            td->cursor_timer++;
            td->cursor_visible = ((td->cursor_timer / CURSOR_BLINK_RATE) & 1)
                                 ? 0 : 1;

            if (td->cursor_visible) {
                int cursor_x = x + INPUT_PAD_X +
                               (td->cursor - td->scroll) * GUI_FONT_W;
                gui_draw_rect(cursor_x, text_y, 2, GUI_FONT_H, GUI_BLUE);
            }
        }
    }

    gui_pop_clip();
}

/* ======================================================================
 * List View drawing
 * ====================================================================== */

static void draw_listview(widget_t *w)
{
    listview_data_t *ld = &w->data.list;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    gui_draw_rect_border(x, y, bw, bh, w->border_color, w->bg_color);

    gui_rect_t clip = { x + 1, y + 1, bw - 2 - SCROLLBAR_W, bh - 2 };
    gui_push_clip(&clip);

    int visible_items = (bh - 2) / LIST_ITEM_H;
    if (visible_items <= 0) visible_items = 1;

    int max_scroll = ld->count - visible_items;
    if (max_scroll < 0) max_scroll = 0;
    if (ld->scroll > max_scroll) ld->scroll = max_scroll;
    if (ld->scroll < 0) ld->scroll = 0;

    /* Draw items with alternating row colors */
    for (int i = 0; i < visible_items && (ld->scroll + i) < ld->count; i++) {
        int idx = ld->scroll + i;
        int iy = y + 1 + i * LIST_ITEM_H;

        uint32_t row_bg;
        uint32_t text_color;

        if (idx == ld->selected) {
            row_bg = GUI_BLUE;
            text_color = GUI_WHITE;
        } else if (idx == ld->hover) {
            row_bg = GUI_BG3;
            text_color = GUI_TEXT;
        } else if (idx & 1) {
            row_bg = GUI_BG2;
            text_color = GUI_TEXT;
        } else {
            row_bg = w->bg_color;
            text_color = GUI_TEXT;
        }

        gui_draw_rect(x + 1, iy, bw - 2 - SCROLLBAR_W,
                      LIST_ITEM_H, row_bg);
        gui_draw_text(x + 8, iy + (LIST_ITEM_H - GUI_FONT_H) / 2,
                      ld->items[idx], text_color);

        if (i < visible_items - 1 && (ld->scroll + i + 1) < ld->count)
            gui_draw_hline(x + 1, iy + LIST_ITEM_H - 1,
                           bw - 2 - SCROLLBAR_W, GUI_BG3);
    }

    gui_pop_clip();

    /* Scrollbar track */
    int sb_x = x + bw - SCROLLBAR_W - 1;
    int sb_y = y + 1;
    int sb_h = bh - 2;
    gui_draw_rect(sb_x, sb_y, SCROLLBAR_W, sb_h, GUI_BG3);

    /* Scrollbar thumb */
    if (ld->count > visible_items && ld->count > 0) {
        int thumb_h = (visible_items * sb_h) / ld->count;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > sb_h) thumb_h = sb_h;

        int thumb_y = sb_y;
        if (max_scroll > 0)
            thumb_y = sb_y + (ld->scroll * (sb_h - thumb_h)) / max_scroll;

        gui_draw_rect(sb_x + 1, thumb_y, SCROLLBAR_W - 2, thumb_h, GUI_TEXT2);
    }
}

/* ======================================================================
 * Chat View drawing
 * ====================================================================== */

static int chatview_compute_height(widget_t *w)
{
    chatview_data_t *cd = &w->data.chat;
    int content_w = w->bounds.w - CHAT_PAD_X * 2 - SCROLLBAR_W - 20;
    if (content_w < 40) content_w = 40;

    int bubble_max_w = (content_w * 75) / 100;
    if (bubble_max_w < 40) bubble_max_w = 40;

    int total_h = CHAT_MSG_GAP;

    for (int i = 0; i < cd->msg_count; i++) {
        int text_h = gui_measure_text_wrap(bubble_max_w - CHAT_PAD_X * 2,
                                            cd->messages[i].text);
        int bubble_h = text_h + CHAT_PAD_Y * 2;
        total_h += bubble_h + CHAT_MSG_GAP;
    }

    return total_h;
}

static void draw_chatview(widget_t *w)
{
    chatview_data_t *cd = &w->data.chat;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    /* Background and border */
    gui_draw_rect(x, y, bw, bh, GUI_BG);
    gui_draw_hline(x, y, bw, w->border_color);
    gui_draw_hline(x, y + bh - 1, bw, w->border_color);
    gui_draw_vline(x, y, bh, w->border_color);
    gui_draw_vline(x + bw - 1, y, bh, w->border_color);

    cd->content_h = chatview_compute_height(w);

    int content_w = bw - SCROLLBAR_W;
    int bubble_max_w = ((content_w - 20) * 75) / 100;
    if (bubble_max_w < 40) bubble_max_w = 40;

    /* Auto-scroll */
    if (cd->auto_scroll) {
        int ms = cd->content_h - bh;
        if (ms < 0) ms = 0;
        cd->scroll = ms;
    }

    int max_scroll = cd->content_h - bh;
    if (max_scroll < 0) max_scroll = 0;
    if (cd->scroll > max_scroll) cd->scroll = max_scroll;
    if (cd->scroll < 0) cd->scroll = 0;

    /* Clip to interior */
    gui_rect_t clip = { x + 1, y + 1, content_w - 2, bh - 2 };
    gui_push_clip(&clip);

    int draw_y = y + CHAT_MSG_GAP - cd->scroll;

    for (int i = 0; i < cd->msg_count; i++) {
        int is_user = cd->messages[i].is_user;
        const char *text = cd->messages[i].text;

        int text_h = gui_measure_text_wrap(bubble_max_w - CHAT_PAD_X * 2,
                                            text);
        int bubble_h = text_h + CHAT_PAD_Y * 2;

        /* Compute bubble width */
        int text_len = gui_strlen(text);
        int text_w_px = text_len * GUI_FONT_W;
        int bubble_w = text_w_px + CHAT_PAD_X * 2;
        if (bubble_w > bubble_max_w) bubble_w = bubble_max_w;
        if (bubble_w < 40) bubble_w = 40;

        /* User messages right-aligned (blue), AI left-aligned (dark bg) */
        int bx;
        uint32_t bubble_bg, text_color;

        if (is_user) {
            bx = x + content_w - bubble_w - 10;
            bubble_bg = GUI_BLUE;
            text_color = GUI_WHITE;
        } else {
            bx = x + 10;
            bubble_bg = GUI_BG3;
            text_color = GUI_TEXT;
        }

        /* Only draw if visible */
        if (draw_y + bubble_h >= y && draw_y < y + bh) {
            gui_draw_rect_border(bx, draw_y, bubble_w, bubble_h,
                                  bubble_bg, bubble_bg);

            gui_draw_text_wrap(bx + CHAT_PAD_X,
                               draw_y + CHAT_PAD_Y,
                               bubble_w - CHAT_PAD_X * 2,
                               text, text_color);
        }

        draw_y += bubble_h + CHAT_MSG_GAP;
    }

    gui_pop_clip();

    /* Scrollbar */
    int sb_x = x + bw - SCROLLBAR_W;
    gui_draw_rect(sb_x, y, SCROLLBAR_W, bh, GUI_BG3);

    if (cd->content_h > bh) {
        int thumb_h = (bh * bh) / cd->content_h;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > bh) thumb_h = bh;

        int thumb_y = y;
        if (max_scroll > 0)
            thumb_y = y + (cd->scroll * (bh - thumb_h)) / max_scroll;

        gui_draw_rect(sb_x + 1, thumb_y, SCROLLBAR_W - 2,
                      thumb_h, GUI_TEXT2);
    }
}

/* ======================================================================
 * Scrollbar drawing
 * ====================================================================== */

static void draw_scrollbar(widget_t *w)
{
    scrollbar_data_t *sb = &w->data.scrollbar;
    int x = w->abs_x;
    int y = w->abs_y;
    int sw = w->bounds.w;
    int sh = w->bounds.h;

    gui_draw_rect(x, y, sw, sh, GUI_BG3);

    if (sb->total <= sb->visible || sb->total <= 0)
        return;

    int track_h = sh;
    int thumb_h = (sb->visible * track_h) / sb->total;
    if (thumb_h < 16) thumb_h = 16;
    if (thumb_h > track_h) thumb_h = track_h;

    int max_pos = sb->total - sb->visible;
    if (max_pos < 1) max_pos = 1;
    int thumb_y = y + (sb->position * (track_h - thumb_h)) / max_pos;

    uint32_t thumb_color = sb->dragging ? GUI_BLUE : GUI_TEXT2;
    gui_draw_rect(x + 1, thumb_y, sw - 2, thumb_h, thumb_color);
}

/* ======================================================================
 * Terminal internals
 * ====================================================================== */

/* Process SGR (Select Graphic Rendition) escape sequence */
static void term_process_sgr(terminal_data_t *td)
{
    if (td->esc_nparam == 0) {
        td->cur_attr = TERM_DEFAULT_ATTR;
        return;
    }
    for (int i = 0; i < td->esc_nparam; i++) {
        int p = td->esc_params[i];
        uint8_t fg = td->cur_attr & 0x0F;
        uint8_t bg = (td->cur_attr >> 4) & 0x0F;

        if (p == 0)           { fg = 7; bg = 0; }
        else if (p == 1)      { fg |= 8; }              /* bold=bright */
        else if (p >= 30 && p <= 37)   { fg = (fg & 8) | (uint8_t)(p - 30); }
        else if (p >= 40 && p <= 47)   { bg = (uint8_t)(p - 40); }
        else if (p >= 90 && p <= 97)   { fg = (uint8_t)(p - 90) + 8; }
        else if (p >= 100 && p <= 107) { bg = (uint8_t)(p - 100) + 8; }

        td->cur_attr = (bg << 4) | fg;
    }
}

/* Scroll the terminal buffer up by one row */
static void term_scroll_up(terminal_data_t *td)
{
    for (int r = 0; r < TERM_BUF_ROWS - 1; r++)
        for (int c = 0; c < TERM_COLS_MAX; c++) {
            g_term_cells[r][c] = g_term_cells[r + 1][c];
            g_term_attrs[r][c] = g_term_attrs[r + 1][c];
        }

    int last = TERM_BUF_ROWS - 1;
    for (int c = 0; c < TERM_COLS_MAX; c++) {
        g_term_cells[last][c] = ' ';
        g_term_attrs[last][c] = TERM_DEFAULT_ATTR;
    }

    if (td->buf_used > TERM_BUF_ROWS)
        td->buf_used = TERM_BUF_ROWS;
    td->cursor_row = TERM_BUF_ROWS - 1;
}

/* Write a raw character to the buffer at cursor position */
static void term_raw_putc(terminal_data_t *td, char c)
{
    if (c == '\n') {
        td->cursor_col = 0;
        td->cursor_row++;
        if (td->cursor_row >= TERM_BUF_ROWS)
            term_scroll_up(td);
        if (td->cursor_row >= td->buf_used)
            td->buf_used = td->cursor_row + 1;
        return;
    }
    if (c == '\r') {
        td->cursor_col = 0;
        return;
    }
    if (c == '\t') {
        int next = (td->cursor_col + 8) & ~7;
        while (td->cursor_col < next && td->cursor_col < td->cols) {
            g_term_cells[td->cursor_row][td->cursor_col] = ' ';
            g_term_attrs[td->cursor_row][td->cursor_col] = td->cur_attr;
            td->cursor_col++;
        }
        if (td->cursor_col >= td->cols) {
            td->cursor_col = 0;
            td->cursor_row++;
            if (td->cursor_row >= TERM_BUF_ROWS)
                term_scroll_up(td);
            if (td->cursor_row >= td->buf_used)
                td->buf_used = td->cursor_row + 1;
        }
        return;
    }

    /* Line wrap */
    if (td->cursor_col >= td->cols) {
        td->cursor_col = 0;
        td->cursor_row++;
        if (td->cursor_row >= TERM_BUF_ROWS)
            term_scroll_up(td);
        if (td->cursor_row >= td->buf_used)
            td->buf_used = td->cursor_row + 1;
    }

    g_term_cells[td->cursor_row][td->cursor_col] = c;
    g_term_attrs[td->cursor_row][td->cursor_col] = td->cur_attr;
    td->cursor_col++;

    if (td->cursor_row >= td->buf_used)
        td->buf_used = td->cursor_row + 1;
}

/* Process a character through the ANSI escape parser */
static void term_process_char(terminal_data_t *td, char c)
{
    switch (td->esc_state) {
    case ESC_NONE:
        if (c == 0x1B)
            td->esc_state = ESC_GOT_ESC;
        else
            term_raw_putc(td, c);
        break;

    case ESC_GOT_ESC:
        if (c == '[') {
            td->esc_state = ESC_CSI;
            td->esc_nparam = 0;
            for (int i = 0; i < 8; i++)
                td->esc_params[i] = 0;
        } else {
            td->esc_state = ESC_NONE;
        }
        break;

    case ESC_CSI:
        if (c >= '0' && c <= '9') {
            if (td->esc_nparam == 0) td->esc_nparam = 1;
            td->esc_params[td->esc_nparam - 1] =
                td->esc_params[td->esc_nparam - 1] * 10 + (uint8_t)(c - '0');
        } else if (c == ';') {
            if (td->esc_nparam < 8) td->esc_nparam++;
        } else {
            if (c == 'm') {
                term_process_sgr(td);
            } else if (c == 'J') {
                /* ESC[2J = clear screen */
                if (td->esc_nparam > 0 && td->esc_params[0] == 2) {
                    for (int r = 0; r < TERM_BUF_ROWS; r++)
                        for (int col = 0; col < TERM_COLS_MAX; col++) {
                            g_term_cells[r][col] = ' ';
                            g_term_attrs[r][col] = TERM_DEFAULT_ATTR;
                        }
                    td->cursor_row = 0;
                    td->cursor_col = 0;
                    td->buf_used = 1;
                    td->scroll = 0;
                }
            } else if (c == 'K') {
                /* ESC[K = clear to end of line */
                for (int col = td->cursor_col; col < TERM_COLS_MAX; col++) {
                    g_term_cells[td->cursor_row][col] = ' ';
                    g_term_attrs[td->cursor_row][col] = TERM_DEFAULT_ATTR;
                }
            }
            td->esc_state = ESC_NONE;
        }
        break;
    }
}

/* Auto-scroll to keep cursor visible */
static void term_auto_scroll(terminal_data_t *td)
{
    int visible = td->rows - 1;   /* -1 for input line */
    if (visible < 1) visible = 1;

    int need = td->buf_used - visible;
    if (need < 0) need = 0;
    if (td->scroll < need)
        td->scroll = need;
}

/* ======================================================================
 * Terminal drawing
 * ====================================================================== */

static void draw_terminal(widget_t *w)
{
    terminal_data_t *td = &w->data.term;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    /* Background */
    gui_draw_rect_border(x, y, bw, bh, w->border_color, 0x0D1117);

    /* Compute grid dimensions */
    int pad = 4;
    td->cols = (bw - pad * 2 - SCROLLBAR_W) / GUI_FONT_W;
    td->rows = (bh - pad * 2) / GUI_FONT_H;
    if (td->cols > TERM_COLS_MAX) td->cols = TERM_COLS_MAX;
    if (td->cols < 1) td->cols = 1;
    if (td->rows < 2) td->rows = 2;

    /* Clip to interior */
    gui_rect_t clip = { x + pad, y + pad,
                        bw - pad * 2 - SCROLLBAR_W, bh - pad * 2 };
    gui_push_clip(&clip);

    term_auto_scroll(td);

    /* Draw output buffer rows */
    int visible_rows = td->rows - 1;  /* reserve bottom row for input */
    for (int r = 0; r < visible_rows; r++) {
        int buf_r = td->scroll + r;
        if (buf_r < 0 || buf_r >= td->buf_used) continue;

        int dy = y + pad + r * GUI_FONT_H;
        for (int c = 0; c < td->cols; c++) {
            char ch = g_term_cells[buf_r][c];
            if (ch == 0 || ch == ' ') continue;

            uint8_t attr = g_term_attrs[buf_r][c];
            uint32_t fg = g_ansi_colors[attr & 0x0F];
            uint32_t bg_c = g_ansi_colors[(attr >> 4) & 0x0F];
            int dx = x + pad + c * GUI_FONT_W;

            if ((attr >> 4) != 0)
                gui_draw_rect(dx, dy, GUI_FONT_W, GUI_FONT_H, bg_c);
            gui_draw_char_transparent(dx, dy, ch, fg);
        }
    }

    /* Draw input line (prompt + current input) on last visible row */
    int input_y = y + pad + visible_rows * GUI_FONT_H;
    int input_x = x + pad;

    /* Prompt */
    for (int i = 0; i < g_term_prompt_len && i < td->cols; i++)
        gui_draw_char_transparent(input_x + i * GUI_FONT_W, input_y,
                                   g_term_prompt[i],
                                   g_ansi_colors[TERM_PROMPT_ATTR & 0x0F]);

    /* Input text */
    int istart = g_term_prompt_len;
    for (int i = 0; i < td->input_len && istart + i < td->cols; i++)
        gui_draw_char_transparent(input_x + (istart + i) * GUI_FONT_W,
                                   input_y, g_term_input[i],
                                   g_ansi_colors[7]);

    /* Blinking cursor */
    if (w->focused) {
        td->cursor_timer++;
        td->cursor_visible = ((td->cursor_timer / CURSOR_BLINK_RATE) & 1)
                             ? 0 : 1;

        if (td->cursor_visible) {
            int cx = input_x + (istart + td->input_cursor) * GUI_FONT_W;
            gui_draw_rect(cx, input_y, GUI_FONT_W, GUI_FONT_H,
                          g_ansi_colors[7]);
            if (td->input_cursor < td->input_len)
                gui_draw_char_transparent(cx, input_y,
                    g_term_input[td->input_cursor], 0x0D1117);
        }
    }

    gui_pop_clip();

    /* Scrollbar */
    int sb_x = x + bw - SCROLLBAR_W;
    gui_draw_rect(sb_x, y, SCROLLBAR_W, bh, GUI_BG3);

    int total = td->buf_used + 1;
    if (total > visible_rows && total > 0) {
        int thumb_h = (visible_rows * bh) / total;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > bh) thumb_h = bh;

        int max_s = total - visible_rows;
        if (max_s < 1) max_s = 1;
        int thumb_y = y + (td->scroll * (bh - thumb_h)) / max_s;

        gui_draw_rect(sb_x + 1, thumb_y, SCROLLBAR_W - 2,
                      thumb_h, GUI_TEXT2);
    }
}

/* ======================================================================
 * Web View — inline Markdown renderer
 * ====================================================================== */

/* Read a line from content at *pos, write into buf (max buflen).
 * Returns line length (0 for blank line, -1 for end). */
static int md_read_line(const char *content, int clen, int *pos,
                        char *buf, int buflen)
{
    if (*pos >= clen) return -1;

    int start = *pos;
    int i = 0;
    while (*pos < clen && content[*pos] != '\n' && i < buflen - 1) {
        buf[i++] = content[(*pos)++];
    }
    buf[i] = '\0';

    if (*pos < clen && content[*pos] == '\n')
        (*pos)++;

    return i;
}

/* Draw a styled line with inline Markdown (**bold**, `code`, [link](url)).
 * Returns height consumed in pixels. */
static int md_draw_inline(int x, int y, int max_w, const char *line,
                           int len, int do_draw, uint32_t base_fg)
{
    int dx = x;
    int dy = y;
    int max_x = x + max_w;
    int bold = 0, italic = 0, code = 0, link_text = 0, link_url = 0;
    uint32_t fg = base_fg;

    for (int i = 0; i < len; i++) {
        char c = line[i];

        /* Skip link URL contents: (url) */
        if (link_url) {
            if (c == ')') link_url = 0;
            continue;
        }

        /* Link text start: [text] */
        if (c == '[' && !code) {
            link_text = 1;
            fg = GUI_BLUE;
            continue;
        }
        if (link_text && c == ']') {
            link_text = 0;
            fg = bold ? GUI_WHITE : (italic ? 0x79C0FF : base_fg);
            if (i + 1 < len && line[i + 1] == '(') {
                link_url = 1;
                i++;
            }
            continue;
        }

        /* Inline code: `text` */
        if (c == '`' && !bold) {
            code = !code;
            fg = code ? GUI_GREEN : base_fg;
            continue;
        }

        /* Bold: **text** */
        if (c == '*' && i + 1 < len && line[i + 1] == '*' && !code) {
            bold = !bold;
            fg = bold ? GUI_WHITE : (italic ? 0x79C0FF : base_fg);
            i++;
            continue;
        }

        /* Italic: *text* (single asterisk, not in code) */
        if (c == '*' && !code) {
            italic = !italic;
            fg = italic ? 0x79C0FF : (bold ? GUI_WHITE : base_fg);
            continue;
        }

        /* Word wrap */
        if (dx + GUI_FONT_W > max_x) {
            dx = x;
            dy += GUI_FONT_H;
        }

        /* Draw character */
        if (do_draw) {
            if (code)
                gui_draw_rect(dx, dy, GUI_FONT_W, GUI_FONT_H, GUI_BG3);
            gui_draw_char_transparent(dx, dy, c, fg);
        }
        dx += GUI_FONT_W;
    }

    return dy - y + GUI_FONT_H;
}

/* Render the full Markdown document. Returns total height. */
static int md_render(widget_t *w, int do_draw)
{
    webview_data_t *wd = &w->data.web;
    int x = w->abs_x + 4;
    int base_y = w->abs_y + 4;
    int max_w = w->bounds.w - 8 - SCROLLBAR_W;
    if (max_w < 40) max_w = 40;

    int pos = 0;
    int dy = base_y - (do_draw ? wd->scroll : 0);
    char line[256];
    int in_code_block = 0;

    while (1) {
        int ll = md_read_line(wd->content, wd->content_len, &pos,
                              line, 256);
        if (ll < 0) break;

        /* Code block fence: ``` */
        if (ll >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') {
            in_code_block = !in_code_block;
            dy += 4;  /* small gap */
            continue;
        }

        /* Inside code block: draw with green on dark bg */
        if (in_code_block) {
            if (do_draw) {
                gui_draw_rect(x, dy, max_w, GUI_FONT_H, GUI_BG3);
                gui_draw_text(x + 4, dy, line, GUI_GREEN);
            }
            dy += GUI_FONT_H;
            continue;
        }

        /* Blank line = paragraph gap */
        if (ll == 0) {
            dy += GUI_FONT_H / 2;
            continue;
        }

        /* Horizontal rule: --- or *** (3+ chars) */
        if (ll >= 3) {
            int is_hr = 1;
            char rc = line[0];
            if (rc == '-' || rc == '*') {
                for (int i = 1; i < ll; i++)
                    if (line[i] != rc && line[i] != ' ') { is_hr = 0; break; }
            } else {
                is_hr = 0;
            }
            if (is_hr) {
                dy += 4;
                if (do_draw)
                    gui_draw_hline(x, dy, max_w, GUI_BORDER);
                dy += 8;
                continue;
            }
        }

        /* Heading: # H1, ## H2, ### H3 */
        if (line[0] == '#') {
            int level = 0;
            while (level < ll && level < 3 && line[level] == '#')
                level++;
            int text_start = level;
            while (text_start < ll && line[text_start] == ' ')
                text_start++;

            uint32_t hfg;
            if (level == 1)      hfg = GUI_BLUE;
            else if (level == 2) hfg = 0x56D4E0;  /* Bright cyan */
            else                 hfg = GUI_WHITE;

            dy += 4;  /* gap before heading */
            int hh = md_draw_inline(x, dy, max_w, line + text_start,
                                     ll - text_start, do_draw, hfg);
            dy += hh;

            /* Underline for H1 */
            if (level == 1 && do_draw) {
                gui_draw_hline(x, dy, max_w, GUI_BLUE);
            }
            if (level == 1) dy += 4;
            dy += 4;  /* gap after heading */
            continue;
        }

        /* Unordered list: - item or * item */
        if (ll >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            int indent = 12;
            if (do_draw) {
                /* Draw bullet */
                int bx = x + 4;
                int by = dy + GUI_FONT_H / 2 - 1;
                gui_draw_rect(bx, by, 3, 3, GUI_TEXT2);
            }
            int lh = md_draw_inline(x + indent, dy, max_w - indent,
                                     line + 2, ll - 2, do_draw, GUI_TEXT);
            dy += lh;
            continue;
        }

        /* Ordered list: 1. item */
        if (ll >= 3 && line[0] >= '1' && line[0] <= '9' && line[1] == '.' &&
            line[2] == ' ') {
            int indent = 20;
            if (do_draw) {
                char num[4] = { line[0], '.', ' ', '\0' };
                gui_draw_text(x + 2, dy, num, GUI_TEXT2);
            }
            int lh = md_draw_inline(x + indent, dy, max_w - indent,
                                     line + 3, ll - 3, do_draw, GUI_TEXT);
            dy += lh;
            continue;
        }

        /* Normal paragraph text */
        int lh = md_draw_inline(x, dy, max_w, line, ll, do_draw, GUI_TEXT);
        dy += lh;
    }

    return dy - base_y + (do_draw ? wd->scroll : 0);
}

static void draw_webview(widget_t *w)
{
    webview_data_t *wd = &w->data.web;
    int x = w->abs_x;
    int y = w->abs_y;
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    /* Background */
    gui_draw_rect_border(x, y, bw, bh, w->border_color, GUI_BG);

    /* Compute content height (measure pass) */
    wd->content_h = md_render(w, 0);

    /* Clamp scroll */
    int max_scroll = wd->content_h - (bh - 8);
    if (max_scroll < 0) max_scroll = 0;
    if (wd->scroll > max_scroll) wd->scroll = max_scroll;
    if (wd->scroll < 0) wd->scroll = 0;

    /* Clip to interior */
    gui_rect_t clip = { x + 1, y + 1, bw - 2 - SCROLLBAR_W, bh - 2 };
    gui_push_clip(&clip);

    /* Draw pass */
    md_render(w, 1);

    gui_pop_clip();

    /* Scrollbar */
    int sb_x = x + bw - SCROLLBAR_W;
    gui_draw_rect(sb_x, y, SCROLLBAR_W, bh, GUI_BG3);

    if (wd->content_h > bh - 8) {
        int thumb_h = ((bh - 8) * bh) / wd->content_h;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > bh) thumb_h = bh;

        int thumb_y = y;
        if (max_scroll > 0)
            thumb_y = y + (wd->scroll * (bh - thumb_h)) / max_scroll;

        gui_draw_rect(sb_x + 1, thumb_y, SCROLLBAR_W - 2,
                      thumb_h, GUI_TEXT2);
    }
}

/* ======================================================================
 * Widget constructors
 * ====================================================================== */

widget_t *widget_panel(int x, int y, int w, int h, const char *title)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_PANEL;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->draw = draw_panel;

    panel_data_t *pd = &wgt->data.panel;
    pd->count = 0;
    pd->show_title = 0;
    pd->title_bg = GUI_BG3;

    if (title && title[0]) {
        gui_strncpy(pd->title, title, WIDGET_TITLE_MAX);
        pd->show_title = 1;
    }

    return wgt;
}

widget_t *widget_label(int x, int y, int w, int h, const char *text)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_LABEL;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->bg_color = 0;
    wgt->draw = draw_label;

    label_data_t *ld = &wgt->data.label;
    ld->align = ALIGN_LEFT;
    ld->wrap = 0;
    if (text)
        gui_strncpy(ld->text, text, WIDGET_TEXT_MAX);

    return wgt;
}

widget_t *widget_button(int x, int y, int w, int h, const char *text,
                         widget_click_fn action)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_BUTTON;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->bg_color = GUI_BG3;
    wgt->draw = draw_button;

    button_data_t *bd = &wgt->data.button;
    bd->hover = 0;
    bd->pressed = 0;
    bd->action = action;
    if (text)
        gui_strncpy(bd->text, text, WIDGET_BUTTON_TEXT_MAX);

    return wgt;
}

widget_t *widget_textinput(int x, int y, int w, int h,
                            const char *placeholder,
                            widget_submit_fn on_submit)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_TEXTINPUT;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->draw = draw_textinput;

    textinput_data_t *td = &wgt->data.input;
    td->cursor = 0;
    td->scroll = 0;
    td->len = 0;
    td->text[0] = '\0';
    td->cursor_visible = 1;
    td->cursor_timer = 0;
    td->on_submit = on_submit;
    if (placeholder)
        gui_strncpy(td->placeholder, placeholder, WIDGET_PLACEHOLDER_MAX);

    return wgt;
}

widget_t *widget_listview(int x, int y, int w, int h,
                           widget_select_fn on_select)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_LISTVIEW;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->draw = draw_listview;

    listview_data_t *ld = &wgt->data.list;
    ld->count = 0;
    ld->selected = -1;
    ld->scroll = 0;
    ld->hover = -1;
    ld->on_select = on_select;

    return wgt;
}

widget_t *widget_chatview(int x, int y, int w, int h)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_CHATVIEW;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->draw = draw_chatview;

    chatview_data_t *cd = &wgt->data.chat;
    cd->msg_count = 0;
    cd->scroll = 0;
    cd->content_h = 0;
    cd->auto_scroll = 1;

    return wgt;
}

widget_t *widget_scrollbar(int x, int y, int w, int h, widget_t *target)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_SCROLLBAR;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->bg_color = GUI_BG3;
    wgt->draw = draw_scrollbar;

    scrollbar_data_t *sd = &wgt->data.scrollbar;
    sd->total = 0;
    sd->visible = h;
    sd->position = 0;
    sd->dragging = 0;
    sd->drag_offset = 0;
    sd->target = target;

    return wgt;
}

widget_t *widget_terminal(int x, int y, int w, int h,
                           terminal_cmd_fn on_command)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_TERMINAL;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->bg_color = 0x0D1117;
    wgt->draw = draw_terminal;

    terminal_data_t *td = &wgt->data.term;
    td->cols = (w - 8 - SCROLLBAR_W) / GUI_FONT_W;
    td->rows = (h - 8) / GUI_FONT_H;
    if (td->cols > TERM_COLS_MAX) td->cols = TERM_COLS_MAX;
    td->cursor_col = 0;
    td->cursor_row = 0;
    td->scroll = 0;
    td->buf_used = 1;
    td->input_len = 0;
    td->input_cursor = 0;
    td->history_count = 0;
    td->history_pos = -1;
    td->cursor_visible = 1;
    td->cursor_timer = 0;
    td->cur_attr = TERM_DEFAULT_ATTR;
    td->esc_state = ESC_NONE;
    td->esc_nparam = 0;
    td->on_command = on_command;

    /* Clear buffers */
    for (int r = 0; r < TERM_BUF_ROWS; r++)
        for (int c = 0; c < TERM_COLS_MAX; c++) {
            g_term_cells[r][c] = ' ';
            g_term_attrs[r][c] = TERM_DEFAULT_ATTR;
        }
    g_term_input[0] = '\0';

    return wgt;
}

widget_t *widget_webview(int x, int y, int w, int h)
{
    widget_t *wgt = widget_alloc();
    if (!wgt) return (widget_t *)0;

    wgt->type = W_WEBVIEW;
    wgt->bounds.x = x;
    wgt->bounds.y = y;
    wgt->bounds.w = w;
    wgt->bounds.h = h;
    wgt->bg_color = GUI_BG;
    wgt->draw = draw_webview;

    webview_data_t *wd = &wgt->data.web;
    wd->content[0] = '\0';
    wd->content_len = 0;
    wd->scroll = 0;
    wd->content_h = 0;

    return wgt;
}

/* ======================================================================
 * Panel operations
 * ====================================================================== */

bool panel_add_child(widget_t *panel, widget_t *child)
{
    if (!panel || panel->type != W_PANEL)
        return false;

    panel_data_t *pd = &panel->data.panel;
    if (pd->count >= WIDGET_MAX_CHILDREN)
        return false;

    pd->children[pd->count++] = child;
    child->parent = panel;
    return true;
}

/* ======================================================================
 * Label operations
 * ====================================================================== */

void label_set_text(widget_t *w, const char *text)
{
    if (!w || w->type != W_LABEL) return;
    gui_strncpy(w->data.label.text, text ? text : "", WIDGET_TEXT_MAX);
}

/* ======================================================================
 * List view operations
 * ====================================================================== */

bool listview_add(widget_t *w, const char *item)
{
    if (!w || w->type != W_LISTVIEW) return false;

    listview_data_t *ld = &w->data.list;
    if (ld->count >= WIDGET_LIST_MAX) return false;

    gui_strncpy(ld->items[ld->count], item ? item : "",
                WIDGET_LIST_ITEM_MAX);
    ld->count++;
    return true;
}

void listview_clear(widget_t *w)
{
    if (!w || w->type != W_LISTVIEW) return;
    w->data.list.count = 0;
    w->data.list.selected = -1;
    w->data.list.scroll = 0;
    w->data.list.hover = -1;
}

void listview_set_selected(widget_t *w, int index)
{
    if (!w || w->type != W_LISTVIEW) return;
    listview_data_t *ld = &w->data.list;
    if (index >= -1 && index < ld->count)
        ld->selected = index;
}

/* ======================================================================
 * Chat view operations
 * ====================================================================== */

void chatview_add(widget_t *w, const char *text, int is_user)
{
    if (!w || w->type != W_CHATVIEW || !text) return;

    chatview_data_t *cd = &w->data.chat;

    if (cd->msg_count >= WIDGET_CHAT_MSG_MAX) {
        for (int i = 0; i < WIDGET_CHAT_MSG_MAX - 1; i++) {
            for (int j = 0; j < WIDGET_CHAT_TEXT_MAX; j++)
                cd->messages[i].text[j] = cd->messages[i + 1].text[j];
            cd->messages[i].is_user = cd->messages[i + 1].is_user;
        }
        cd->msg_count = WIDGET_CHAT_MSG_MAX - 1;
    }

    gui_strncpy(cd->messages[cd->msg_count].text, text,
                WIDGET_CHAT_TEXT_MAX);
    cd->messages[cd->msg_count].is_user = is_user;
    cd->msg_count++;

    cd->content_h = chatview_compute_height(w);

    if (cd->auto_scroll)
        chatview_scroll_to_bottom(w);
}

void chatview_scroll_to_bottom(widget_t *w)
{
    if (!w || w->type != W_CHATVIEW) return;

    chatview_data_t *cd = &w->data.chat;
    int ms = cd->content_h - w->bounds.h;
    if (ms < 0) ms = 0;
    cd->scroll = ms;
    cd->auto_scroll = 1;
}

void chatview_clear(widget_t *w)
{
    if (!w || w->type != W_CHATVIEW) return;
    w->data.chat.msg_count = 0;
    w->data.chat.scroll = 0;
    w->data.chat.content_h = 0;
}

/* ======================================================================
 * Text input operations
 * ====================================================================== */

void textinput_clear(widget_t *w)
{
    if (!w || w->type != W_TEXTINPUT) return;
    w->data.input.text[0] = '\0';
    w->data.input.cursor = 0;
    w->data.input.scroll = 0;
    w->data.input.len = 0;
    w->data.input.cursor_timer = 0;
    w->data.input.cursor_visible = 1;
}

void textinput_set_text(widget_t *w, const char *text)
{
    if (!w || w->type != W_TEXTINPUT) return;
    textinput_data_t *td = &w->data.input;
    gui_strncpy(td->text, text ? text : "", WIDGET_INPUT_MAX);
    td->len = gui_strlen(td->text);
    td->cursor = td->len;
    td->scroll = 0;
}

const char *textinput_get_text(widget_t *w)
{
    if (!w || w->type != W_TEXTINPUT) return "";
    return w->data.input.text;
}

/* ======================================================================
 * Web view operations
 * ====================================================================== */

void webview_set_content(widget_t *w, const char *markdown)
{
    if (!w || w->type != W_WEBVIEW || !markdown) return;
    webview_data_t *wd = &w->data.web;
    gui_strncpy(wd->content, markdown, WEBVIEW_CONTENT_MAX);
    wd->content_len = gui_strlen(wd->content);
    wd->scroll = 0;
    wd->content_h = 0;
}

void webview_append(widget_t *w, const char *text)
{
    if (!w || w->type != W_WEBVIEW || !text) return;
    webview_data_t *wd = &w->data.web;
    int tlen = gui_strlen(text);
    int space = WEBVIEW_CONTENT_MAX - 1 - wd->content_len;
    if (space <= 0) return;
    if (tlen > space) tlen = space;
    for (int i = 0; i < tlen; i++)
        wd->content[wd->content_len + i] = text[i];
    wd->content_len += tlen;
    wd->content[wd->content_len] = '\0';
}

void webview_clear(widget_t *w)
{
    if (!w || w->type != W_WEBVIEW) return;
    w->data.web.content[0] = '\0';
    w->data.web.content_len = 0;
    w->data.web.scroll = 0;
    w->data.web.content_h = 0;
}

void webview_scroll_top(widget_t *w)
{
    if (!w || w->type != W_WEBVIEW) return;
    w->data.web.scroll = 0;
}

/* ======================================================================
 * Terminal operations
 * ====================================================================== */

void terminal_putc(widget_t *w, char c)
{
    if (!w || w->type != W_TERMINAL) return;
    term_process_char(&w->data.term, c);
}

void terminal_puts(widget_t *w, const char *s)
{
    if (!w || w->type != W_TERMINAL || !s) return;
    terminal_data_t *td = &w->data.term;
    while (*s)
        term_process_char(td, *s++);
}

void terminal_clear(widget_t *w)
{
    if (!w || w->type != W_TERMINAL) return;
    terminal_data_t *td = &w->data.term;
    for (int r = 0; r < TERM_BUF_ROWS; r++)
        for (int c = 0; c < TERM_COLS_MAX; c++) {
            g_term_cells[r][c] = ' ';
            g_term_attrs[r][c] = TERM_DEFAULT_ATTR;
        }
    td->cursor_row = 0;
    td->cursor_col = 0;
    td->buf_used = 1;
    td->scroll = 0;
    td->cur_attr = TERM_DEFAULT_ATTR;
    td->esc_state = ESC_NONE;
}

void terminal_set_prompt(const char *prompt)
{
    if (!prompt) return;
    gui_strncpy(g_term_prompt, prompt, TERM_PROMPT_MAX);
    g_term_prompt_len = gui_strlen(g_term_prompt);
}

void terminal_set_color(widget_t *w, uint8_t attr)
{
    if (!w || w->type != W_TERMINAL) return;
    w->data.term.cur_attr = attr;
}

/* ======================================================================
 * Widget tree drawing
 * ====================================================================== */

void widget_draw(widget_t *w)
{
    if (!w || !w->visible)
        return;

    widget_compute_abs(w);

    if (w->draw)
        w->draw(w);
}

/* ======================================================================
 * Hit testing
 * ====================================================================== */

static bool widget_contains(widget_t *w, int sx, int sy)
{
    return sx >= w->abs_x && sx < w->abs_x + w->bounds.w &&
           sy >= w->abs_y && sy < w->abs_y + w->bounds.h;
}

widget_t *widget_hit_test(widget_t *root, int x, int y)
{
    if (!root || !root->visible)
        return (widget_t *)0;

    widget_compute_abs(root);

    if (!widget_contains(root, x, y))
        return (widget_t *)0;

    if (root->type == W_PANEL) {
        panel_data_t *pd = &root->data.panel;
        for (int i = pd->count - 1; i >= 0; i--) {
            widget_t *child = pd->children[i];
            if (!child || !child->visible) continue;

            widget_t *hit = widget_hit_test(child, x, y);
            if (hit) return hit;
        }
    }

    return root;
}

/* ======================================================================
 * Clear hover state
 * ====================================================================== */

static void widget_clear_hover(widget_t *w)
{
    if (!w) return;

    if (w->type == W_BUTTON)
        w->data.button.hover = 0;

    if (w->type == W_LISTVIEW)
        w->data.list.hover = -1;

    if (w->type == W_PANEL) {
        panel_data_t *pd = &w->data.panel;
        for (int i = 0; i < pd->count; i++) {
            if (pd->children[i])
                widget_clear_hover(pd->children[i]);
        }
    }
}

/* ======================================================================
 * Mouse event dispatch
 * ====================================================================== */

widget_t *widget_mouse_event(widget_t *root, gui_event_t *evt)
{
    if (!root || !evt) return (widget_t *)0;

    if (evt->type == EVT_MOUSE_MOVE)
        widget_clear_hover(root);

    widget_t *target = widget_hit_test(root, evt->mx, evt->my);
    if (!target) return (widget_t *)0;

    /* Button */
    if (target->type == W_BUTTON) {
        button_data_t *bd = &target->data.button;

        if (evt->type == EVT_MOUSE_MOVE)
            bd->hover = 1;

        if (evt->type == EVT_MOUSE_DOWN) {
            bd->pressed = 1;
            bd->hover = 1;
        }

        if (evt->type == EVT_MOUSE_UP) {
            if (bd->pressed && bd->action && target->enabled)
                bd->action();
            bd->pressed = 0;
        }

        if (evt->type == EVT_MOUSE_CLICK) {
            if (bd->action && target->enabled)
                bd->action();
        }
    }

    /* TextInput */
    if (target->type == W_TEXTINPUT) {
        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK) {
            widget_set_focus(target);

            textinput_data_t *td = &target->data.input;
            int rel_x = evt->mx - target->abs_x - INPUT_PAD_X;
            int char_pos = td->scroll + (rel_x / GUI_FONT_W);
            if (char_pos < 0) char_pos = 0;
            if (char_pos > td->len) char_pos = td->len;
            td->cursor = char_pos;
            td->cursor_timer = 0;
            td->cursor_visible = 1;
        }
    }

    /* ListView */
    if (target->type == W_LISTVIEW) {
        listview_data_t *ld = &target->data.list;
        int rel_y = evt->my - target->abs_y - 1;
        int item_idx = ld->scroll + (rel_y / LIST_ITEM_H);

        if (evt->type == EVT_MOUSE_MOVE) {
            if (item_idx >= 0 && item_idx < ld->count)
                ld->hover = item_idx;
            else
                ld->hover = -1;
        }

        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK) {
            if (item_idx >= 0 && item_idx < ld->count) {
                ld->selected = item_idx;
                if (ld->on_select)
                    ld->on_select(item_idx, ld->items[item_idx]);
            }
            widget_set_focus(target);
        }
    }

    /* ChatView */
    if (target->type == W_CHATVIEW) {
        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK) {
            widget_set_focus(target);
            target->data.chat.auto_scroll = 0;
        }
    }

    /* Terminal */
    if (target->type == W_TERMINAL) {
        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK) {
            widget_set_focus(target);
            target->data.term.cursor_timer = 0;
            target->data.term.cursor_visible = 1;
        }
    }

    /* WebView */
    if (target->type == W_WEBVIEW) {
        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK)
            widget_set_focus(target);
    }

    /* Scrollbar */
    if (target->type == W_SCROLLBAR) {
        scrollbar_data_t *sb = &target->data.scrollbar;

        if (sb->total > sb->visible && sb->total > 0) {
            int track_h = target->bounds.h;
            int thumb_h = (sb->visible * track_h) / sb->total;
            if (thumb_h < 16) thumb_h = 16;
            int max_pos = sb->total - sb->visible;
            if (max_pos < 1) max_pos = 1;
            int thumb_y = target->abs_y +
                          (sb->position * (track_h - thumb_h)) / max_pos;

            if (evt->type == EVT_MOUSE_DOWN) {
                if (evt->my >= thumb_y && evt->my < thumb_y + thumb_h) {
                    sb->dragging = 1;
                    sb->drag_offset = evt->my - thumb_y;
                } else {
                    int new_y = evt->my - thumb_h / 2 - target->abs_y;
                    if (new_y < 0) new_y = 0;
                    if (new_y > track_h - thumb_h)
                        new_y = track_h - thumb_h;
                    sb->position = (new_y * max_pos) / (track_h - thumb_h);
                }
            }

            if (evt->type == EVT_MOUSE_MOVE && sb->dragging) {
                int new_y = evt->my - sb->drag_offset - target->abs_y;
                if (new_y < 0) new_y = 0;
                if (new_y > track_h - thumb_h)
                    new_y = track_h - thumb_h;
                sb->position = (new_y * max_pos) / (track_h - thumb_h);
            }

            if (evt->type == EVT_MOUSE_UP)
                sb->dragging = 0;
        }
    }

    /* Custom handler */
    if (target->on_click &&
        (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK)) {
        target->on_click(target, evt->mx - target->abs_x,
                         evt->my - target->abs_y);
    }

    return target;
}

/* ======================================================================
 * Keyboard event dispatch
 * ====================================================================== */

bool widget_key_event(widget_t *focused, gui_event_t *evt)
{
    if (!focused || !evt || evt->type != EVT_KEY_DOWN)
        return false;

    int key = evt->key;

    /* --- Text input --- */
    if (focused->type == W_TEXTINPUT) {
        textinput_data_t *td = &focused->data.input;

        td->cursor_timer = 0;
        td->cursor_visible = 1;

        if (key == '\n' || key == '\r') {
            if (td->on_submit && td->len > 0)
                td->on_submit(td->text);
            return true;
        }

        if (key == '\b' || key == 127) {
            if (td->cursor > 0) {
                for (int i = td->cursor - 1; i < td->len; i++)
                    td->text[i] = td->text[i + 1];
                td->cursor--;
                td->len--;
            }
            return true;
        }

        if (key == GUI_KEY_DELETE) {
            if (td->cursor < td->len) {
                for (int i = td->cursor; i < td->len; i++)
                    td->text[i] = td->text[i + 1];
                td->len--;
            }
            return true;
        }

        if (key == GUI_KEY_HOME) { td->cursor = 0; return true; }
        if (key == GUI_KEY_END)  { td->cursor = td->len; return true; }

        if (key == GUI_KEY_LEFT) {
            if (td->cursor > 0) td->cursor--;
            return true;
        }

        if (key == GUI_KEY_RIGHT) {
            if (td->cursor < td->len) td->cursor++;
            return true;
        }

        if (key == GUI_KEY_ESCAPE) {
            textinput_clear(focused);
            return true;
        }

        if (key == GUI_KEY_TAB || key == '\t')
            return false;

        if (key >= 0x20 && key <= 0x7E) {
            if (td->len < WIDGET_INPUT_MAX - 1) {
                for (int i = td->len; i >= td->cursor; i--)
                    td->text[i + 1] = td->text[i];
                td->text[td->cursor] = (char)key;
                td->cursor++;
                td->len++;
            }
            return true;
        }

        return false;
    }

    /* --- List view --- */
    if (focused->type == W_LISTVIEW) {
        listview_data_t *ld = &focused->data.list;

        if (key == GUI_KEY_UP) {
            if (ld->selected > 0) {
                ld->selected--;
                if (ld->selected < ld->scroll)
                    ld->scroll = ld->selected;
                if (ld->on_select)
                    ld->on_select(ld->selected, ld->items[ld->selected]);
            }
            return true;
        }

        if (key == GUI_KEY_DOWN) {
            if (ld->selected < ld->count - 1) {
                ld->selected++;
                int visible = (focused->bounds.h - 2) / LIST_ITEM_H;
                if (ld->selected >= ld->scroll + visible)
                    ld->scroll = ld->selected - visible + 1;
                if (ld->on_select)
                    ld->on_select(ld->selected, ld->items[ld->selected]);
            }
            return true;
        }

        if (key == GUI_KEY_HOME) {
            ld->selected = 0;
            ld->scroll = 0;
            if (ld->on_select && ld->count > 0)
                ld->on_select(0, ld->items[0]);
            return true;
        }

        if (key == GUI_KEY_END) {
            if (ld->count > 0) {
                ld->selected = ld->count - 1;
                int visible = (focused->bounds.h - 2) / LIST_ITEM_H;
                int ms = ld->count - visible;
                if (ms < 0) ms = 0;
                ld->scroll = ms;
                if (ld->on_select)
                    ld->on_select(ld->selected, ld->items[ld->selected]);
            }
            return true;
        }

        if (key == GUI_KEY_PGUP) {
            int visible = (focused->bounds.h - 2) / LIST_ITEM_H;
            ld->selected -= visible;
            if (ld->selected < 0) ld->selected = 0;
            if (ld->selected < ld->scroll)
                ld->scroll = ld->selected;
            if (ld->on_select && ld->count > 0)
                ld->on_select(ld->selected, ld->items[ld->selected]);
            return true;
        }

        if (key == GUI_KEY_PGDN) {
            int visible = (focused->bounds.h - 2) / LIST_ITEM_H;
            ld->selected += visible;
            if (ld->selected >= ld->count)
                ld->selected = ld->count - 1;
            if (ld->selected >= ld->scroll + visible)
                ld->scroll = ld->selected - visible + 1;
            if (ld->on_select && ld->count > 0)
                ld->on_select(ld->selected, ld->items[ld->selected]);
            return true;
        }

        return false;
    }

    /* --- Chat view --- */
    if (focused->type == W_CHATVIEW) {
        chatview_data_t *cd = &focused->data.chat;
        int scroll_step = GUI_FONT_H * 3;

        if (key == GUI_KEY_UP || key == GUI_KEY_PGUP) {
            if (key == GUI_KEY_PGUP)
                scroll_step = focused->bounds.h - GUI_FONT_H;
            cd->scroll -= scroll_step;
            if (cd->scroll < 0) cd->scroll = 0;
            cd->auto_scroll = 0;
            return true;
        }

        if (key == GUI_KEY_DOWN || key == GUI_KEY_PGDN) {
            if (key == GUI_KEY_PGDN)
                scroll_step = focused->bounds.h - GUI_FONT_H;
            cd->scroll += scroll_step;
            int ms = cd->content_h - focused->bounds.h;
            if (ms < 0) ms = 0;
            if (cd->scroll >= ms) {
                cd->scroll = ms;
                cd->auto_scroll = 1;
            }
            return true;
        }

        if (key == GUI_KEY_HOME) {
            cd->scroll = 0;
            cd->auto_scroll = 0;
            return true;
        }

        if (key == GUI_KEY_END) {
            chatview_scroll_to_bottom(focused);
            return true;
        }

        return false;
    }

    /* --- Web View --- */
    if (focused->type == W_WEBVIEW) {
        webview_data_t *wd = &focused->data.web;
        int scroll_step = GUI_FONT_H * 3;
        int max_s = wd->content_h - (focused->bounds.h - 8);
        if (max_s < 0) max_s = 0;

        if (key == GUI_KEY_UP) {
            wd->scroll -= scroll_step;
            if (wd->scroll < 0) wd->scroll = 0;
            return true;
        }
        if (key == GUI_KEY_DOWN) {
            wd->scroll += scroll_step;
            if (wd->scroll > max_s) wd->scroll = max_s;
            return true;
        }
        if (key == GUI_KEY_PGUP) {
            wd->scroll -= focused->bounds.h - GUI_FONT_H;
            if (wd->scroll < 0) wd->scroll = 0;
            return true;
        }
        if (key == GUI_KEY_PGDN) {
            wd->scroll += focused->bounds.h - GUI_FONT_H;
            if (wd->scroll > max_s) wd->scroll = max_s;
            return true;
        }
        if (key == GUI_KEY_HOME) {
            wd->scroll = 0;
            return true;
        }
        if (key == GUI_KEY_END) {
            wd->scroll = max_s;
            return true;
        }
        if (key == GUI_KEY_TAB || key == '\t')
            return false;
        return false;
    }

    /* --- Terminal --- */
    if (focused->type == W_TERMINAL) {
        terminal_data_t *td = &focused->data.term;

        td->cursor_timer = 0;
        td->cursor_visible = 1;

        /* Enter: execute command */
        if (key == '\n' || key == '\r') {
            g_term_input[td->input_len] = '\0';

            /* Echo prompt + input to buffer */
            uint8_t save = td->cur_attr;
            td->cur_attr = TERM_PROMPT_ATTR;
            for (int i = 0; i < g_term_prompt_len; i++)
                term_raw_putc(td, g_term_prompt[i]);
            td->cur_attr = TERM_DEFAULT_ATTR;
            for (int i = 0; i < td->input_len; i++)
                term_raw_putc(td, g_term_input[i]);
            term_raw_putc(td, '\n');
            td->cur_attr = save;

            /* Add to history */
            if (td->input_len > 0) {
                if (td->history_count >= TERM_HISTORY_MAX) {
                    for (int h = 0; h < TERM_HISTORY_MAX - 1; h++)
                        gui_strncpy(g_term_history[h],
                                    g_term_history[h + 1], TERM_INPUT_MAX);
                    td->history_count = TERM_HISTORY_MAX - 1;
                }
                gui_strncpy(g_term_history[td->history_count],
                            g_term_input, TERM_INPUT_MAX);
                td->history_count++;
            }

            /* Execute */
            if (td->on_command && td->input_len > 0)
                td->on_command(g_term_input);

            /* Reset input */
            td->input_len = 0;
            td->input_cursor = 0;
            td->history_pos = -1;
            g_term_input[0] = '\0';
            return true;
        }

        /* Backspace */
        if (key == '\b' || key == 127) {
            if (td->input_cursor > 0) {
                for (int i = td->input_cursor - 1; i < td->input_len; i++)
                    g_term_input[i] = g_term_input[i + 1];
                td->input_cursor--;
                td->input_len--;
            }
            return true;
        }

        /* Delete */
        if (key == GUI_KEY_DELETE) {
            if (td->input_cursor < td->input_len) {
                for (int i = td->input_cursor; i < td->input_len; i++)
                    g_term_input[i] = g_term_input[i + 1];
                td->input_len--;
            }
            return true;
        }

        /* History: Up */
        if (key == GUI_KEY_UP) {
            if (td->history_count > 0) {
                if (td->history_pos == -1) {
                    gui_strncpy(g_term_input_save, g_term_input,
                                TERM_INPUT_MAX);
                    g_term_input_save_len = td->input_len;
                    td->history_pos = td->history_count - 1;
                } else if (td->history_pos > 0) {
                    td->history_pos--;
                }
                gui_strncpy(g_term_input,
                            g_term_history[td->history_pos], TERM_INPUT_MAX);
                td->input_len = gui_strlen(g_term_input);
                td->input_cursor = td->input_len;
            }
            return true;
        }

        /* History: Down */
        if (key == GUI_KEY_DOWN) {
            if (td->history_pos >= 0) {
                td->history_pos++;
                if (td->history_pos >= td->history_count) {
                    td->history_pos = -1;
                    gui_strncpy(g_term_input, g_term_input_save,
                                TERM_INPUT_MAX);
                    td->input_len = g_term_input_save_len;
                } else {
                    gui_strncpy(g_term_input,
                                g_term_history[td->history_pos],
                                TERM_INPUT_MAX);
                    td->input_len = gui_strlen(g_term_input);
                }
                td->input_cursor = td->input_len;
            }
            return true;
        }

        /* Cursor movement */
        if (key == GUI_KEY_LEFT) {
            if (td->input_cursor > 0) td->input_cursor--;
            return true;
        }
        if (key == GUI_KEY_RIGHT) {
            if (td->input_cursor < td->input_len) td->input_cursor++;
            return true;
        }
        if (key == GUI_KEY_HOME) { td->input_cursor = 0; return true; }
        if (key == GUI_KEY_END)  { td->input_cursor = td->input_len; return true; }

        /* Page up/down: scroll output */
        if (key == GUI_KEY_PGUP) {
            td->scroll -= (td->rows - 2);
            if (td->scroll < 0) td->scroll = 0;
            return true;
        }
        if (key == GUI_KEY_PGDN) {
            td->scroll += (td->rows - 2);
            int ms = td->buf_used - (td->rows - 1);
            if (ms < 0) ms = 0;
            if (td->scroll > ms) td->scroll = ms;
            return true;
        }

        /* Tab: let desktop handle focus cycling */
        if (key == GUI_KEY_TAB || key == '\t')
            return false;

        /* Printable character */
        if (key >= 0x20 && key <= 0x7E) {
            if (td->input_len < TERM_INPUT_MAX - 1) {
                for (int i = td->input_len; i >= td->input_cursor; i--)
                    g_term_input[i + 1] = g_term_input[i];
                g_term_input[td->input_cursor] = (char)key;
                td->input_cursor++;
                td->input_len++;
            }
            return true;
        }

        return false;
    }

    /* Custom handler */
    if (focused->on_key) {
        focused->on_key(focused, key);
        return true;
    }

    return false;
}
