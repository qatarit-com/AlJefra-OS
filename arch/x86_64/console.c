/* SPDX-License-Identifier: MIT */
/* AlJefra OS — x86-64 Console HAL Implementation
 * Dual output: serial COM1 (0x3F8) + linear framebuffer (from multiboot).
 * On real hardware (UEFI), serial may not be visible — the LFB provides
 * on-screen text output using the bitmap font in drivers/display/lfb.c.
 */

#include "../../hal/hal.h"
#include "../../drivers/display/lfb.h"
#include <stdarg.h>

/* -------------------------------------------------------------------------- */
/* Multiboot info structure (subset of fields we need)                        */
/* -------------------------------------------------------------------------- */

/* Multiboot info flags bits */
#define MB_INFO_FRAMEBUFFER  (1u << 12)

/* Multiboot info structure layout (from multiboot spec) */
typedef struct {
    uint32_t flags;             /* 0 */
    uint32_t mem_lower;         /* 4 */
    uint32_t mem_upper;         /* 8 */
    uint32_t boot_device;       /* 12 */
    uint32_t cmdline;           /* 16 */
    uint32_t mods_count;        /* 20 */
    uint32_t mods_addr;         /* 24 */
    uint32_t syms[4];           /* 28-40 */
    uint32_t mmap_length;       /* 44 */
    uint32_t mmap_addr;         /* 48 */
    uint32_t drives_length;     /* 52 */
    uint32_t drives_addr;       /* 56 */
    uint32_t config_table;      /* 60 */
    uint32_t boot_loader_name;  /* 64 */
    uint32_t apm_table;         /* 68 */
    /* VBE info */
    uint32_t vbe_control_info;  /* 72 */
    uint32_t vbe_mode_info;     /* 76 */
    uint16_t vbe_mode;          /* 80 */
    uint16_t vbe_interface_seg; /* 82 */
    uint16_t vbe_interface_off; /* 84 */
    uint16_t vbe_interface_len; /* 86 */
    /* Framebuffer info (GRUB extension, flags bit 12) */
    uint64_t framebuffer_addr;  /* 88 */
    uint32_t framebuffer_pitch; /* 96 */
    uint32_t framebuffer_width; /* 100 */
    uint32_t framebuffer_height;/* 104 */
    uint8_t  framebuffer_bpp;   /* 108 */
    uint8_t  framebuffer_type;  /* 109: 0=indexed, 1=RGB, 2=EGA text */
} __attribute__((packed)) multiboot_info_t;

/* Saved by boot.S */
extern uint64_t boot_mb_info_ptr;
extern uint32_t boot_mb_magic;

/* -------------------------------------------------------------------------- */
/* Serial port (COM1) constants                                               */
/* -------------------------------------------------------------------------- */

#define COM1_BASE       0x3F8
#define COM1_DATA       (COM1_BASE + 0)
#define COM1_IER        (COM1_BASE + 1)
#define COM1_FCR        (COM1_BASE + 2)
#define COM1_LCR        (COM1_BASE + 3)
#define COM1_MCR        (COM1_BASE + 4)
#define COM1_LSR        (COM1_BASE + 5)
#define COM1_MSR        (COM1_BASE + 6)
#define COM1_DLL        (COM1_BASE + 0)
#define COM1_DLH        (COM1_BASE + 1)

#define LSR_DATA_READY  (1u << 0)
#define LSR_TX_EMPTY    (1u << 5)

/* -------------------------------------------------------------------------- */
/* Port I/O helpers                                                           */
/* -------------------------------------------------------------------------- */

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* -------------------------------------------------------------------------- */
/* Console state                                                              */
/* -------------------------------------------------------------------------- */

static hal_console_type_t current_type = HAL_CONSOLE_SERIAL;
static bool console_initialized = false;
static bool serial_available = false;
static bool lfb_available = false;
static lfb_console_t lfb_con;

/* -------------------------------------------------------------------------- */
/* Serial I/O                                                                 */
/* -------------------------------------------------------------------------- */

static void serial_init(void)
{
    outb(COM1_IER, 0x00);
    outb(COM1_LCR, 0x80);
    outb(COM1_DLL, 0x01);
    outb(COM1_DLH, 0x00);
    outb(COM1_LCR, 0x03);
    outb(COM1_FCR, 0xC7);
    outb(COM1_MCR, 0x0B);

    /* Loopback test */
    outb(COM1_MCR, 0x1E);
    outb(COM1_DATA, 0xAE);
    if (inb(COM1_DATA) == 0xAE)
        serial_available = true;

    outb(COM1_MCR, 0x0B);
}

static void serial_putc(char c)
{
    while (!(inb(COM1_LSR) & LSR_TX_EMPTY))
        __asm__ volatile ("pause");
    outb(COM1_DATA, (uint8_t)c);
}

static int serial_has_data(void)
{
    return (inb(COM1_LSR) & LSR_DATA_READY) != 0;
}

static char serial_getc(void)
{
    return (char)inb(COM1_DATA);
}

/* -------------------------------------------------------------------------- */
/* Framebuffer init from multiboot info                                       */
/* -------------------------------------------------------------------------- */

static void framebuffer_init(void)
{
    if (boot_mb_magic != 0x2BADB002)
        return;   /* Not booted via multiboot */

    multiboot_info_t *mb = (multiboot_info_t *)(uintptr_t)boot_mb_info_ptr;
    if (!mb)
        return;

    /* Check if framebuffer info is available (flags bit 12) */
    if (!(mb->flags & MB_INFO_FRAMEBUFFER))
        return;

    /* Only support RGB direct color framebuffer (type 1) */
    if (mb->framebuffer_type != 1)
        return;

    if (mb->framebuffer_addr == 0 || mb->framebuffer_width == 0 ||
        mb->framebuffer_height == 0 || mb->framebuffer_bpp == 0)
        return;

    lfb_info_t fb;
    fb.base      = (volatile void *)(uintptr_t)mb->framebuffer_addr;
    fb.phys_base = mb->framebuffer_addr;
    fb.width     = mb->framebuffer_width;
    fb.height    = mb->framebuffer_height;
    fb.pitch     = mb->framebuffer_pitch;
    fb.bpp       = mb->framebuffer_bpp;

    if (lfb_init(&lfb_con, &fb) == HAL_OK)
        lfb_available = true;
}

/* -------------------------------------------------------------------------- */
/* HAL Console API                                                            */
/* -------------------------------------------------------------------------- */

hal_status_t hal_console_init(void)
{
    serial_init();
    framebuffer_init();

    if (lfb_available) {
        current_type = HAL_CONSOLE_LFB;
    } else if (serial_available) {
        current_type = HAL_CONSOLE_SERIAL;
    } else {
        current_type = HAL_CONSOLE_VGA;
    }

    console_initialized = true;
    return HAL_OK;
}

void hal_console_putc(char c)
{
    /* Always output to serial if available (for debugging) */
    if (serial_available) {
        if (c == '\n')
            serial_putc('\r');
        serial_putc(c);
    }

    /* Also output to framebuffer if available (for screen display) */
    if (lfb_available) {
        lfb_putc(&lfb_con, c);
    }
}

void hal_console_puts(const char *s)
{
    if (!s) return;

    while (*s) {
        hal_console_putc(*s);
        s++;
    }
}

void hal_console_write(const char *s, uint64_t len)
{
    if (!s || len == 0) return;

    for (uint64_t i = 0; i < len; i++)
        hal_console_putc(s[i]);
}

/* -------------------------------------------------------------------------- */
/* Minimal printf implementation                                              */
/* Supports: %s, %d, %u, %x, %p, %c, %l (as prefix for long), %%            */
/* -------------------------------------------------------------------------- */

static void print_decimal(int64_t val)
{
    if (val < 0) {
        hal_console_putc('-');
        val = -val;
    }

    char buf[21];
    int pos = 0;

    if (val == 0) {
        hal_console_putc('0');
        return;
    }

    while (val > 0) {
        buf[pos++] = '0' + (char)(val % 10);
        val /= 10;
    }

    for (int i = pos - 1; i >= 0; i--)
        hal_console_putc(buf[i]);
}

static void print_unsigned(uint64_t val)
{
    char buf[21];
    int pos = 0;

    if (val == 0) {
        hal_console_putc('0');
        return;
    }

    while (val > 0) {
        buf[pos++] = '0' + (char)(val % 10);
        val /= 10;
    }

    for (int i = pos - 1; i >= 0; i--)
        hal_console_putc(buf[i]);
}

static void print_hex(uint64_t val, int width)
{
    static const char hex_digits[] = "0123456789abcdef";

    if (width == 0) {
        if (val == 0) {
            hal_console_putc('0');
            return;
        }
        char buf[17];
        int pos = 0;
        while (val > 0) {
            buf[pos++] = hex_digits[val & 0xF];
            val >>= 4;
        }
        for (int i = pos - 1; i >= 0; i--)
            hal_console_putc(buf[i]);
    } else {
        for (int i = (width - 1) * 4; i >= 0; i -= 4)
            hal_console_putc(hex_digits[(val >> i) & 0xF]);
    }
}

void hal_console_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            hal_console_putc(*fmt);
            fmt++;
            continue;
        }

        fmt++;

        int zero_pad = 0;
        if (*fmt == '0') {
            zero_pad = 1;
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l')
                fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            hal_console_puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t val;
            if (is_long)
                val = va_arg(args, int64_t);
            else
                val = va_arg(args, int);
            print_decimal(val);
            break;
        }
        case 'u': {
            uint64_t val;
            if (is_long)
                val = va_arg(args, uint64_t);
            else
                val = va_arg(args, unsigned int);
            print_unsigned(val);
            break;
        }
        case 'x': {
            uint64_t val;
            if (is_long)
                val = va_arg(args, uint64_t);
            else
                val = va_arg(args, unsigned int);
            print_hex(val, (zero_pad && width > 0) ? width : 0);
            break;
        }
        case 'p': {
            void *ptr = va_arg(args, void *);
            hal_console_puts("0x");
            print_hex((uint64_t)(uintptr_t)ptr, 16);
            break;
        }
        case '%':
            hal_console_putc('%');
            break;
        case 'c': {
            char c = (char)va_arg(args, int);
            hal_console_putc(c);
            break;
        }
        case '\0':
            goto done;
        default:
            hal_console_putc('%');
            hal_console_putc(*fmt);
            break;
        }

        fmt++;
    }

done:
    va_end(args);
}

char hal_console_getc(void)
{
    if (serial_available && serial_has_data())
        return serial_getc();
    return 0;
}

int hal_console_has_input(void)
{
    if (serial_available)
        return serial_has_data();
    return 0;
}

hal_console_type_t hal_console_type(void)
{
    return current_type;
}

void hal_console_set_uart_base(uint64_t mmio_base)
{
    (void)mmio_base;
}
