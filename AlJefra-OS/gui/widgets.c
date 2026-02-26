/* SPDX-License-Identifier: MIT */
/* AlJefra OS -- Widget Toolkit Implementation
 * ~2100 lines.  Complete widget drawing, event handling, and layout.
 *
 * All widgets are statically allocated from a fixed pool.
 * No malloc, no dynamic memory -- runs in kernel space.
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
        return (widget_t *)0;  /* Pool exhausted */

    widget_t *w = &g_widget_pool[g_widget_count++];

    /* Zero-initialize the entire widget */
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

    if (w)
        w->focused = 1;
}

widget_t *widget_get_focus(void)
{
    return g_focused_widget;
}

/* ======================================================================
 * Internal draw helpers
 * ====================================================================== */

/* Compute absolute position by walking up the parent chain */
static void widget_compute_abs(widget_t *w)
{
    w->abs_x = w->bounds.x;
    w->abs_y = w->bounds.y;

    widget_t *p = w->parent;
    while (p) {
        w->abs_x += p->abs_x;
        w->abs_y += p->abs_y;

        /* If parent is a panel with a title bar, offset by title height */
        if (p->type == W_PANEL && p->data.panel.show_title)
            w->abs_y += 28;  /* title bar height */

        p = p->parent;
    }
}

/* Title bar height for panels */
#define PANEL_TITLE_H  28

/* List view item height */
#define LIST_ITEM_H    24

/* Chat message padding */
#define CHAT_PAD_X     10
#define CHAT_PAD_Y     6
#define CHAT_MSG_GAP   8

/* Scrollbar width */
#define SCROLLBAR_W    10

/* Text input padding */
#define INPUT_PAD_X    6
#define INPUT_PAD_Y    4

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

    /* Panel background with border */
    gui_draw_rect_border(x, y, bw, bh, w->border_color, w->bg_color);

    /* Title bar */
    if (pd->show_title) {
        uint32_t tbg = pd->title_bg ? pd->title_bg : GUI_BG3;
        gui_draw_rect(x + 1, y + 1, bw - 2, PANEL_TITLE_H - 1, tbg);
        gui_draw_hline(x + 1, y + PANEL_TITLE_H, bw - 2, w->border_color);

        /* Title text (centered vertically in title bar) */
        int tx = x + 10;
        int ty = y + (PANEL_TITLE_H - GUI_FONT_H) / 2;
        gui_draw_text(tx, ty, pd->title, GUI_TEXT);
    }

    /* Set clip to panel interior */
    int interior_y = y + (pd->show_title ? PANEL_TITLE_H + 1 : 1);
    int interior_h = bh - (pd->show_title ? PANEL_TITLE_H + 2 : 2);
    gui_rect_t clip = { x + 1, interior_y, bw - 2, interior_h };
    gui_push_clip(&clip);

    /* Draw children */
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

    uint32_t color = w->fg_color;

    if (ld->wrap) {
        gui_draw_text_wrap(x, y, w->bounds.w, ld->text, color);
    } else {
        int text_len = gui_strlen(ld->text);
        int text_w = text_len * GUI_FONT_W;
        int tx = x;

        if (ld->align == ALIGN_CENTER)
            tx = x + (w->bounds.w - text_w) / 2;
        else if (ld->align == ALIGN_RIGHT)
            tx = x + w->bounds.w - text_w;

        int ty = y + (w->bounds.h - GUI_FONT_H) / 2;
        gui_draw_text(tx, ty, ld->text, color);
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

    /* Determine colors based on state */
    uint32_t bg, fg, border;

    if (bd->pressed) {
        bg = GUI_BLUE;
        fg = GUI_WHITE;
        border = GUI_BLUE;
    } else if (bd->hover) {
        bg = GUI_BG3;
        fg = GUI_WHITE;
        border = GUI_BLUE;
    } else {
        bg = w->bg_color;
        fg = w->fg_color;
        border = w->border_color;
    }

    /* Draw button body */
    gui_draw_rect_border(x, y, bw, bh, border, bg);

    /* Draw text centered */
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

    /* Blink cursor every 30 frames */
    if (w->focused) {
        td->cursor_timer++;
        if (td->cursor_timer >= 30) {
            td->cursor_timer = 0;
            td->cursor_visible = !td->cursor_visible;
        }
    } else {
        td->cursor_visible = 0;
        td->cursor_timer = 0;
    }

    /* Border color changes when focused */
    uint32_t border = w->focused ? GUI_BLUE : w->border_color;
    gui_draw_rect_border(x, y, bw, bh, border, GUI_BG);

    /* Clip to interior */
    gui_rect_t clip = { x + INPUT_PAD_X, y + 1,
                        bw - INPUT_PAD_X * 2, bh - 2 };
    gui_push_clip(&clip);

    int text_y = y + (bh - GUI_FONT_H) / 2;

    if (td->len == 0 && !w->focused) {
        /* Show placeholder */
        gui_draw_text(x + INPUT_PAD_X, text_y,
                      td->placeholder, GUI_TEXT2);
    } else {
        /* Compute visible range */
        int visible_chars = (bw - INPUT_PAD_X * 2) / GUI_FONT_W;
        if (visible_chars <= 0) visible_chars = 1;

        /* Ensure cursor is visible */
        if (td->cursor < td->scroll)
            td->scroll = td->cursor;
        if (td->cursor >= td->scroll + visible_chars)
            td->scroll = td->cursor - visible_chars + 1;
        if (td->scroll < 0)
            td->scroll = 0;

        /* Draw visible text */
        int draw_x = x + INPUT_PAD_X;
        for (int i = td->scroll; i < td->len && i < td->scroll + visible_chars; i++) {
            gui_draw_char_transparent(draw_x, text_y,
                                       td->text[i], w->fg_color);
            draw_x += GUI_FONT_W;
        }

        /* Draw cursor */
        if (w->focused && td->cursor_visible) {
            int cursor_x = x + INPUT_PAD_X +
                           (td->cursor - td->scroll) * GUI_FONT_W;
            gui_draw_rect(cursor_x, text_y, 2, GUI_FONT_H, GUI_BLUE);
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

    /* Background */
    gui_draw_rect_border(x, y, bw, bh, w->border_color, w->bg_color);

    /* Clip to interior */
    gui_rect_t clip = { x + 1, y + 1, bw - 2 - SCROLLBAR_W, bh - 2 };
    gui_push_clip(&clip);

    /* Compute visible range */
    int visible_items = (bh - 2) / LIST_ITEM_H;
    if (visible_items <= 0) visible_items = 1;

    /* Clamp scroll */
    int max_scroll = ld->count - visible_items;
    if (max_scroll < 0) max_scroll = 0;
    if (ld->scroll > max_scroll) ld->scroll = max_scroll;
    if (ld->scroll < 0) ld->scroll = 0;

    /* Draw items */
    for (int i = 0; i < visible_items && (ld->scroll + i) < ld->count; i++) {
        int idx = ld->scroll + i;
        int iy = y + 1 + i * LIST_ITEM_H;

        /* Highlight selected item */
        if (idx == ld->selected) {
            gui_draw_rect(x + 1, iy, bw - 2 - SCROLLBAR_W,
                          LIST_ITEM_H, GUI_BLUE);
            gui_draw_text(x + 8, iy + (LIST_ITEM_H - GUI_FONT_H) / 2,
                          ld->items[idx], GUI_WHITE);
        } else if (idx == ld->hover) {
            gui_draw_rect(x + 1, iy, bw - 2 - SCROLLBAR_W,
                          LIST_ITEM_H, GUI_BG3);
            gui_draw_text(x + 8, iy + (LIST_ITEM_H - GUI_FONT_H) / 2,
                          ld->items[idx], GUI_TEXT);
        } else {
            gui_draw_text(x + 8, iy + (LIST_ITEM_H - GUI_FONT_H) / 2,
                          ld->items[idx], GUI_TEXT);
        }

        /* Separator line */
        if (i < visible_items - 1 && (ld->scroll + i + 1) < ld->count)
            gui_draw_hline(x + 1, iy + LIST_ITEM_H - 1,
                           bw - 2 - SCROLLBAR_W, GUI_BG3);
    }

    gui_pop_clip();

    /* Draw scrollbar track */
    int sb_x = x + bw - SCROLLBAR_W - 1;
    int sb_y = y + 1;
    int sb_h = bh - 2;
    gui_draw_rect(sb_x, sb_y, SCROLLBAR_W, sb_h, GUI_BG3);

    /* Draw scrollbar thumb */
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

/* Compute total height of all chat messages */
static int chatview_compute_height(widget_t *w)
{
    chatview_data_t *cd = &w->data.chat;
    int content_w = w->bounds.w - CHAT_PAD_X * 2 - SCROLLBAR_W - 20;
    if (content_w < 40) content_w = 40;

    /* Bubble max width: 70% of content area */
    int bubble_max_w = (content_w * 70) / 100;
    if (bubble_max_w < 40) bubble_max_w = 40;

    int total_h = CHAT_MSG_GAP;

    for (int i = 0; i < cd->msg_count; i++) {
        int text_h = gui_measure_text_wrap(bubble_max_w - CHAT_PAD_X * 2,
                                            cd->messages[i].text);
        int bubble_h = text_h + CHAT_PAD_Y * 2;
        /* Add space for sender label */
        total_h += GUI_FONT_H + 2 + bubble_h + CHAT_MSG_GAP;
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

    /* Background */
    gui_draw_rect(x, y, bw, bh, GUI_BG);

    /* Compute content height */
    cd->content_h = chatview_compute_height(w);

    int content_w = bw - SCROLLBAR_W;
    int bubble_max_w = ((content_w - 20) * 70) / 100;
    if (bubble_max_w < 40) bubble_max_w = 40;

    /* Auto-scroll: if at bottom, keep at bottom */
    int max_scroll = cd->content_h - bh;
    if (max_scroll < 0) max_scroll = 0;

    if (cd->auto_scroll) {
        cd->scroll = max_scroll;
    }

    /* Clamp scroll */
    if (cd->scroll > max_scroll) cd->scroll = max_scroll;
    if (cd->scroll < 0) cd->scroll = 0;

    /* Clip to interior */
    gui_rect_t clip = { x, y, content_w, bh };
    gui_push_clip(&clip);

    /* Draw messages */
    int draw_y = y + CHAT_MSG_GAP - cd->scroll;

    for (int i = 0; i < cd->msg_count; i++) {
        int is_user = cd->messages[i].is_user;
        const char *text = cd->messages[i].text;

        int text_h = gui_measure_text_wrap(bubble_max_w - CHAT_PAD_X * 2,
                                            text);
        int bubble_h = text_h + CHAT_PAD_Y * 2;

        /* Compute actual bubble width from text length */
        int text_len = gui_strlen(text);
        int text_w_px = text_len * GUI_FONT_W;
        int bubble_w = text_w_px + CHAT_PAD_X * 2;
        if (bubble_w > bubble_max_w) bubble_w = bubble_max_w;
        if (bubble_w < 60) bubble_w = 60;

        /* Position: user messages on right, AI on left */
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

        /* Only draw if visible on screen */
        int msg_total_h = GUI_FONT_H + 2 + bubble_h;
        if (draw_y + msg_total_h >= y && draw_y < y + bh) {
            /* Draw sender label */
            if (is_user) {
                gui_draw_text(bx + bubble_w - 24, draw_y,
                              "You", GUI_TEXT2);
            } else {
                gui_draw_text(bx, draw_y, "AI", GUI_GREEN);
            }

            int label_h = GUI_FONT_H + 2;

            /* Draw bubble background */
            gui_draw_rect_border(bx, draw_y + label_h, bubble_w, bubble_h,
                                  bubble_bg, bubble_bg);

            /* Draw message text inside bubble */
            gui_draw_text_wrap(bx + CHAT_PAD_X,
                               draw_y + label_h + CHAT_PAD_Y,
                               bubble_w - CHAT_PAD_X * 2,
                               text, text_color);
        }

        draw_y += msg_total_h + CHAT_MSG_GAP;
    }

    gui_pop_clip();

    /* Draw scrollbar */
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
    int bw = w->bounds.w;
    int bh = w->bounds.h;

    /* Track */
    gui_draw_rect(x, y, bw, bh, GUI_BG3);

    /* Thumb */
    if (sb->total > sb->visible && sb->total > 0) {
        int thumb_h = (sb->visible * bh) / sb->total;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > bh) thumb_h = bh;

        int max_pos = sb->total - sb->visible;
        if (max_pos <= 0) max_pos = 1;

        int thumb_y = y + (sb->position * (bh - thumb_h)) / max_pos;

        gui_draw_rect(x + 1, thumb_y, bw - 2, thumb_h, GUI_TEXT2);
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
    wgt->bg_color = 0;  /* Transparent background */
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
    wgt->draw = draw_scrollbar;

    scrollbar_data_t *sb = &wgt->data.scrollbar;
    sb->total = 0;
    sb->visible = 0;
    sb->position = 0;
    sb->dragging = 0;
    sb->drag_offset = 0;
    sb->target = target;

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

    /* If buffer full, shift everything up by one */
    if (cd->msg_count >= WIDGET_CHAT_MSG_MAX) {
        for (int i = 0; i < WIDGET_CHAT_MSG_MAX - 1; i++) {
            /* Copy text */
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

    /* Recompute content height */
    cd->content_h = chatview_compute_height(w);

    /* Auto-scroll to bottom */
    if (cd->auto_scroll)
        chatview_scroll_to_bottom(w);
}

void chatview_scroll_to_bottom(widget_t *w)
{
    if (!w || w->type != W_CHATVIEW) return;

    chatview_data_t *cd = &w->data.chat;
    int max_scroll = cd->content_h - w->bounds.h;
    if (max_scroll < 0) max_scroll = 0;
    cd->scroll = max_scroll;
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
 * Widget tree drawing
 * ====================================================================== */

void widget_draw(widget_t *w)
{
    if (!w || !w->visible)
        return;

    /* Compute absolute position */
    widget_compute_abs(w);

    /* Call the type-specific draw function */
    if (w->draw)
        w->draw(w);
}

/* ======================================================================
 * Hit testing
 * ====================================================================== */

/* Check if point is inside widget's absolute bounds */
static bool widget_contains(widget_t *w, int sx, int sy)
{
    return sx >= w->abs_x && sx < w->abs_x + w->bounds.w &&
           sy >= w->abs_y && sy < w->abs_y + w->bounds.h;
}

widget_t *widget_hit_test(widget_t *root, int x, int y)
{
    if (!root || !root->visible)
        return (widget_t *)0;

    /* Compute absolute position */
    widget_compute_abs(root);

    if (!widget_contains(root, x, y))
        return (widget_t *)0;

    /* For panels, check children (in reverse order for z-order) */
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
 * Mouse event dispatch
 * ====================================================================== */

/* Clear all button hover/press state in a widget tree */
static void widget_clear_hover(widget_t *w)
{
    if (!w) return;

    if (w->type == W_BUTTON) {
        w->data.button.hover = 0;
    }

    if (w->type == W_LISTVIEW) {
        w->data.list.hover = -1;
    }

    if (w->type == W_PANEL) {
        panel_data_t *pd = &w->data.panel;
        for (int i = 0; i < pd->count; i++) {
            if (pd->children[i])
                widget_clear_hover(pd->children[i]);
        }
    }
}

widget_t *widget_mouse_event(widget_t *root, gui_event_t *evt)
{
    if (!root || !evt) return (widget_t *)0;

    /* Clear hover state on mouse move */
    if (evt->type == EVT_MOUSE_MOVE)
        widget_clear_hover(root);

    /* Hit test to find the target widget */
    widget_t *target = widget_hit_test(root, evt->mx, evt->my);
    if (!target) return (widget_t *)0;

    /* --- Button hover/press handling --- */
    if (target->type == W_BUTTON) {
        button_data_t *bd = &target->data.button;

        if (evt->type == EVT_MOUSE_MOVE) {
            bd->hover = 1;
        }

        if (evt->type == EVT_MOUSE_DOWN) {
            bd->pressed = 1;
            bd->hover = 1;
        }

        if (evt->type == EVT_MOUSE_UP) {
            if (bd->pressed && bd->action)
                bd->action();
            bd->pressed = 0;
        }
    }

    /* --- Text input click-to-focus and cursor placement --- */
    if (target->type == W_TEXTINPUT) {
        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK) {
            widget_set_focus(target);

            /* Place cursor near click position */
            textinput_data_t *td = &target->data.input;
            int rel_x = evt->mx - target->abs_x - INPUT_PAD_X;
            int char_pos = td->scroll + (rel_x / GUI_FONT_W);
            if (char_pos < 0) char_pos = 0;
            if (char_pos > td->len) char_pos = td->len;
            td->cursor = char_pos;
            td->cursor_visible = 1;
            td->cursor_timer = 0;
        }
    }

    /* --- List view click handling --- */
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

    /* --- Chat view scroll handling --- */
    if (target->type == W_CHATVIEW) {
        chatview_data_t *cd = &target->data.chat;

        if (evt->type == EVT_MOUSE_DOWN || evt->type == EVT_MOUSE_CLICK) {
            widget_set_focus(target);
            cd->auto_scroll = 0;  /* User interaction disables auto-scroll */
        }
    }

    /* Call custom handler if set */
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

    /* --- Text input keyboard handling --- */
    if (focused->type == W_TEXTINPUT) {
        textinput_data_t *td = &focused->data.input;

        /* Reset cursor blink on any keypress */
        td->cursor_visible = 1;
        td->cursor_timer = 0;

        /* Enter: submit */
        if (key == '\n' || key == '\r') {
            if (td->on_submit && td->len > 0)
                td->on_submit(td->text);
            return true;
        }

        /* Backspace */
        if (key == '\b' || key == 127) {
            if (td->cursor > 0) {
                /* Shift chars left */
                for (int i = td->cursor - 1; i < td->len; i++)
                    td->text[i] = td->text[i + 1];
                td->cursor--;
                td->len--;
            }
            return true;
        }

        /* Delete */
        if (key == GUI_KEY_DELETE) {
            if (td->cursor < td->len) {
                for (int i = td->cursor; i < td->len; i++)
                    td->text[i] = td->text[i + 1];
                td->len--;
            }
            return true;
        }

        /* Home */
        if (key == GUI_KEY_HOME) {
            td->cursor = 0;
            return true;
        }

        /* End */
        if (key == GUI_KEY_END) {
            td->cursor = td->len;
            return true;
        }

        /* Left arrow */
        if (key == GUI_KEY_LEFT) {
            if (td->cursor > 0)
                td->cursor--;
            return true;
        }

        /* Right arrow */
        if (key == GUI_KEY_RIGHT) {
            if (td->cursor < td->len)
                td->cursor++;
            return true;
        }

        /* Escape: clear */
        if (key == GUI_KEY_ESCAPE) {
            textinput_clear(focused);
            return true;
        }

        /* Tab: move focus (handled by caller) */
        if (key == GUI_KEY_TAB || key == '\t') {
            return false;  /* Not consumed */
        }

        /* Printable character */
        if (key >= 0x20 && key <= 0x7E) {
            if (td->len < WIDGET_INPUT_MAX - 1) {
                /* Shift chars right */
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

    /* --- List view keyboard handling --- */
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
                int max_s = ld->count - visible;
                if (max_s < 0) max_s = 0;
                ld->scroll = max_s;
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
            if (ld->selected < 0) ld->selected = 0;
            if (ld->selected >= ld->scroll + visible)
                ld->scroll = ld->selected - visible + 1;
            if (ld->on_select && ld->count > 0)
                ld->on_select(ld->selected, ld->items[ld->selected]);
            return true;
        }

        return false;
    }

    /* --- Chat view keyboard handling --- */
    if (focused->type == W_CHATVIEW) {
        chatview_data_t *cd = &focused->data.chat;
        int scroll_step = GUI_FONT_H * 3;  /* Scroll 3 lines at a time */

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
            int max_s = cd->content_h - focused->bounds.h;
            if (max_s < 0) max_s = 0;
            if (cd->scroll >= max_s) {
                cd->scroll = max_s;
                cd->auto_scroll = 1;  /* Reached bottom */
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
            cd->auto_scroll = 1;
            return true;
        }

        return false;
    }

    /* Call custom handler if set */
    if (focused->on_key) {
        focused->on_key(focused, key);
        return true;
    }

    return false;
}
