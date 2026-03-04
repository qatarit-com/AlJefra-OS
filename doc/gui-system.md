# AlJefra OS -- Desktop GUI System

## Overview

The AlJefra OS desktop environment is a downloadable `.ajdrv` plugin, not a built-in kernel component. At boot time, if a display adapter is detected via PCIe enumeration or EFI GOP, the AI bootstrap offers to download and install the GUI plugin from the marketplace. This keeps the kernel minimal while providing a full graphical experience when display hardware is available.

The GUI system totals 4,705 lines across its core modules and provides a complete desktop shell with a widget toolkit, window management, and integrated AI chat interface.

## Core Graphics Layer

**Source**: `gui/gui.c`

### Framebuffer Architecture

The graphics core operates directly on a linear framebuffer provided by the display subsystem (VESA VBE or EFI GOP):

- **Pixel format**: 32-bit ARGB (8 bits per channel, alpha in high byte)
- **Double-buffering**: All drawing occurs in a back buffer; the completed frame is copied to the visible framebuffer in a single `memcpy` operation to prevent tearing
- **Clipping**: All draw operations are clipped to the current widget bounds, preventing overdraw outside designated regions

### Drawing Primitives

```c
void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_rect(int x, int y, int w, int h, uint32_t color);
void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gui_draw_char(int x, int y, char c, uint32_t color);
void gui_draw_text(int x, int y, const char *text, uint32_t color);
void gui_blit(int x, int y, const uint32_t *pixels, int w, int h);
void gui_flip(void);  // Swap back buffer to front
```

### Resolution

The framebuffer resolution is auto-detected from the firmware:

- **VESA VBE**: Mode information block queried during boot
- **EFI GOP**: Graphics Output Protocol provides mode list and current resolution
- **Fallback**: 1024x768 if detection fails

## Widget Toolkit

**Source**: `gui/widgets.c`

The widget toolkit provides a retained-mode UI framework. Widgets are allocated, configured, and arranged into a tree. The toolkit handles hit testing, event dispatch, and rendering.

### Widget Types

#### Panel

Container widget with background fill and optional border. Panels are the primary layout mechanism -- all other widgets are placed inside panels.

```c
widget_t *panel = widget_create_panel(x, y, width, height);
panel->bg_color = 0xFF0D1117;    // Dark background
panel->border_color = 0xFF30363D; // Subtle border
```

#### Label

Static text rendering with font support. Labels support single-line and multi-line text with automatic line wrapping within bounds.

```c
widget_t *label = widget_create_label(x, y, "System Information");
label->text_color = 0xFFE6EDF3;  // Light text
```

#### Button

Interactive element with click handler and visual hover/press states. Buttons receive mouse events and invoke a callback on click.

```c
widget_t *btn = widget_create_button(x, y, w, h, "Send");
btn->on_click = handle_send_click;
// Visual states: normal, hover (lighter bg), pressed (darker bg)
```

#### TextInput

Editable text field with cursor display, text selection basics, and keyboard input handling. Used for the AI chat input box, file rename dialogs, and WiFi password entry.

```c
widget_t *input = widget_create_textinput(x, y, w, h);
input->placeholder = "Type a message...";
input->on_submit = handle_text_submit;  // Enter key
```

#### ListView

Scrollable vertical list of items. Each item can display text and an optional icon. ListView supports selection highlighting and scroll events.

```c
widget_t *list = widget_create_listview(x, y, w, h);
listview_add_item(list, "config.txt", ICON_FILE);
listview_add_item(list, "notes.txt", ICON_FILE);
list->on_select = handle_file_selected;
```

#### ChatView

Specialized widget for displaying AI chat conversations. Renders message bubbles with distinct styling for user messages and AI responses. Supports automatic scrolling to the latest message.

```c
widget_t *chat = widget_create_chatview(x, y, w, h);
chatview_add_message(chat, "show my files", CHAT_USER);
chatview_add_message(chat, "Here are your files:\n  config.txt\n  notes.txt", CHAT_AI);
// Auto-scrolls to bottom on new messages
```

#### Scrollbar

Vertical scrollbar with drag support. Automatically attached to ListView and ChatView widgets. The thumb size reflects the visible-to-total content ratio.

```c
widget_t *scrollbar = widget_create_scrollbar(x, y, h);
scrollbar->total_items = 100;
scrollbar->visible_items = 20;
scrollbar->on_scroll = handle_scroll_position;
```

## Desktop Shell

**Source**: `gui/desktop.c`

The desktop shell assembles the widget toolkit into a complete user interface with multiple panels.

### Top Bar

A fixed-height bar at the top of the screen displaying system status:

```
+--------------------------------------------------------------------+
|  AlJefra OS        WiFi: Connected (HomeNet)    14:32    EN        |
+--------------------------------------------------------------------+
```

- **OS name**: "AlJefra OS" branding on the left
- **Network status**: Connection type and network name (or "Disconnected")
- **Clock**: Current time in HH:MM format, updated every minute
- **Language indicator**: "EN" or "AR" showing the current input language

### File Browser

A panel displaying the contents of the BMFS filesystem:

- Directory listing from `fs_list()` with filename, size, and block count
- File selection with single-click highlighting
- File operations: Open (read and display), Delete (with confirmation)
- File info display: name, size in human-readable format, allocated blocks

### AI Chat Window

The primary interaction interface, occupying the largest portion of the screen:

- **Message display**: ChatView widget showing the conversation history with styled bubbles
- **Input box**: TextInput widget at the bottom for typing messages
- **Send button**: Button widget to submit the message (Enter key also works)
- **Auto-scroll**: New messages automatically scroll the view to the bottom

### Settings Panel

System configuration interface with tabbed sections:

| Tab | Controls |
|-----|----------|
| Network | WiFi scan/connect, view IP address, DNS settings |
| Display | Resolution selection, brightness (if supported) |
| Language | Switch between English and Arabic |
| AI Provider | Select LLM backend (AlJefra Cloud / Claude / Offline) |

### Terminal

A power-user panel providing direct command-line access to the OS. Commands typed here are processed by the kernel's command handler rather than the AI chat engine, allowing direct system interaction for advanced users.

## Theme

The GUI uses a dark theme that matches the AlJefra website palette:

```css
/* Color palette */
--bg-primary:    #0D1117;  /* Main background */
--bg-secondary:  #161B22;  /* Panel backgrounds */
--bg-tertiary:   #21262D;  /* Input fields, hover states */
--border:        #30363D;  /* Panel borders, dividers */
--text-primary:  #E6EDF3;  /* Primary text */
--text-secondary:#8B949E;  /* Secondary/muted text */
--accent:        #58A6FF;  /* Links, active elements, buttons */
--accent-hover:  #79C0FF;  /* Button hover state */
--success:       #3FB950;  /* Success indicators */
--warning:       #D29922;  /* Warning indicators */
--error:         #F85149;  /* Error indicators */
```

All widgets draw using these palette values. The theme is consistent across every panel and widget, providing a cohesive visual identity.

## Layout

The desktop uses a tiling window manager approach. Panels are arranged in a fixed grid layout rather than freely draggable windows:

```
+------------------------------------------------------------------+
|                         Top Bar                                   |
+----------------+---------------------------------+----------------+
|                |                                 |                |
|  File Browser  |         AI Chat Window          |   Settings     |
|                |                                 |   Panel        |
|                |                                 |                |
|                |                                 |                |
|                +---------------------------------+                |
|                |          Terminal               |                |
+----------------+---------------------------------+----------------+
```

Panels can be resized by dragging dividers. The layout adapts to the detected screen resolution.

## Input Handling

### Keyboard

- **PS/2 keyboard**: Scan codes translated to keycodes via the PS/2 driver
- **USB HID keyboard**: Keycodes received via the xHCI + USB HID driver stack
- **Key events**: Dispatched to the focused widget (TextInput, Terminal)
- **Shortcuts**: Ctrl+C (copy), Ctrl+V (paste), Tab (focus next widget)

### Mouse

- **Cursor rendering**: Software sprite drawn on the back buffer before flip
- **Hit testing**: Mouse coordinates tested against widget bounds to determine the target
- **Events**: Move (hover state), Button down (press state), Button up (click)
- **Scroll wheel**: Mapped to ListView and ChatView scroll events

## Rendering Pipeline

```
1. Clear back buffer (bg-primary fill)
2. Render top bar (OS name, status, clock)
3. Render each panel:
   a. Fill panel background
   b. Draw panel border
   c. Render child widgets (labels, buttons, lists, etc.)
4. Render mouse cursor sprite
5. gui_flip() -- copy back buffer to visible framebuffer
6. Repeat at ~30 FPS (or on-demand when input events occur)
```

The renderer is event-driven when idle (redraws only on input events) and switches to continuous rendering during animations (scroll, chat message arrival).
