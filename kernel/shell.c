/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Interactive Shell
 *
 * Provides a basic command-line interface after boot.
 * Uses keyboard_getchar() for input (works via UEFI legacy PS/2
 * emulation on modern laptops, or native PS/2, or USB HID).
 */

#include "shell.h"
#include "keyboard.h"
#include "driver_loader.h"
#include "dhcp.h"
#include "sched.h"
#include "../hal/hal.h"
#include "../lib/string.h"

/* ── Command line buffer ── */
#define SHELL_LINE_MAX  128
static char g_line[SHELL_LINE_MAX];
static uint32_t g_line_pos;

/* ── Boot timestamp (set when shell starts) ── */
static uint64_t g_boot_ms;

/* ── Device list (passed from kernel_main) ── */
static hal_device_t *g_devices;
static uint32_t      g_device_count;

/* ── Forward declarations ── */
static void cmd_help(void);
static void cmd_info(void);
static void cmd_pci(void);
static void cmd_mem(void);
static void cmd_clear(void);
static void cmd_drivers(void);
static void cmd_uptime(void);
static void cmd_reboot(void);
static void cmd_ver(void);
static void cmd_net(void);

/* ── String comparison (simple, no libc) ── */
static int sh_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ── Prompt ── */
static void shell_prompt(void)
{
    hal_console_puts("aljefra> ");
}

/* ── Execute a command ── */
static void shell_exec(const char *cmd)
{
    /* Skip leading spaces */
    while (*cmd == ' ') cmd++;

    /* Empty line */
    if (*cmd == '\0') return;

    if (sh_streq(cmd, "help") || sh_streq(cmd, "?"))
        cmd_help();
    else if (sh_streq(cmd, "info") || sh_streq(cmd, "uname"))
        cmd_info();
    else if (sh_streq(cmd, "pci") || sh_streq(cmd, "lspci"))
        cmd_pci();
    else if (sh_streq(cmd, "mem") || sh_streq(cmd, "free"))
        cmd_mem();
    else if (sh_streq(cmd, "clear") || sh_streq(cmd, "cls"))
        cmd_clear();
    else if (sh_streq(cmd, "drivers") || sh_streq(cmd, "lsmod"))
        cmd_drivers();
    else if (sh_streq(cmd, "uptime"))
        cmd_uptime();
    else if (sh_streq(cmd, "reboot"))
        cmd_reboot();
    else if (sh_streq(cmd, "version") || sh_streq(cmd, "ver"))
        cmd_ver();
    else if (sh_streq(cmd, "net") || sh_streq(cmd, "network") || sh_streq(cmd, "ip"))
        cmd_net();
    else {
        hal_console_puts("I don't recognize '");
        hal_console_puts(cmd);
        hal_console_puts("'. Type 'help' to see what I can do.\n");
    }
}

/* ── Main shell loop ── */
void shell_run(void)
{
    g_boot_ms = hal_timer_ms();
    g_line_pos = 0;

    hal_console_puts("\n");
    hal_console_puts("Welcome! AlJefra OS is ready.\n");
    hal_console_puts("Type 'help' to see what you can do, or 'net' to check your connection.\n\n");
    shell_prompt();

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            hal_console_putc('\n');
            g_line[g_line_pos] = '\0';
            shell_exec(g_line);
            g_line_pos = 0;
            shell_prompt();
        } else if (c == '\b' || c == 0x7F) {
            /* Backspace */
            if (g_line_pos > 0) {
                g_line_pos--;
                /* Erase character on screen: back, space, back */
                hal_console_putc('\b');
                hal_console_putc(' ');
                hal_console_putc('\b');
            }
        } else if (c >= ' ' && g_line_pos < SHELL_LINE_MAX - 1) {
            g_line[g_line_pos++] = c;
            hal_console_putc(c);
        }
    }
}

/* ── Set device list (called by kernel_main before shell_run) ── */
void shell_set_devices(hal_device_t *devs, uint32_t count)
{
    g_devices = devs;
    g_device_count = count;
}

/* ── Command implementations ── */

static void cmd_help(void)
{
    hal_console_puts("Here's what you can do:\n\n");
    hal_console_puts("  help      - Show this guide\n");
    hal_console_puts("  info      - About this computer (CPU, RAM, architecture)\n");
    hal_console_puts("  net       - Check internet connection and IP address\n");
    hal_console_puts("  pci       - Show connected hardware devices\n");
    hal_console_puts("  mem       - How much memory is available\n");
    hal_console_puts("  drivers   - What hardware drivers are running\n");
    hal_console_puts("  uptime    - How long the system has been on\n");
    hal_console_puts("  clear     - Clear the screen\n");
    hal_console_puts("  version   - AlJefra OS version info\n");
    hal_console_puts("  reboot    - Restart the computer\n");
    hal_console_puts("\nExample: type 'net' and press Enter to see your IP address.\n");
}

static void cmd_info(void)
{
    hal_cpu_info_t cpu;
    hal_cpu_get_info(&cpu);

    hal_console_puts("AlJefra OS v0.7.0\n");
    hal_console_puts("Architecture: ");
    switch (hal_arch()) {
    case HAL_ARCH_X86_64:  hal_console_puts("x86-64\n");  break;
    case HAL_ARCH_AARCH64: hal_console_puts("AArch64\n"); break;
    case HAL_ARCH_RISCV64: hal_console_puts("RISC-V 64\n"); break;
    }
    hal_console_puts("Processor:    ");
    hal_console_puts(cpu.model);
    hal_console_puts("\n");
    hal_console_printf("CPU cores:    %u\n", cpu.cores_logical);
    hal_console_printf("Memory:       %u MB\n",
        (uint32_t)(hal_mmu_total_ram() / (1024 * 1024)));
}

static void cmd_pci(void)
{
    if (!g_devices || g_device_count == 0) {
        hal_console_puts("No hardware devices detected.\n");
        return;
    }

    hal_console_printf("Found %u hardware devices:\n", g_device_count);
    for (uint32_t i = 0; i < g_device_count; i++) {
        hal_device_t *d = &g_devices[i];
        if (d->bus_type == HAL_BUS_PCIE) {
            hal_console_printf("  %02x:%02x.%x  %04x:%04x  class %02x:%02x",
                d->bus, d->dev, d->func,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass);
            /* Label known classes */
            if (d->class_code == 0x01) hal_console_puts("  [Storage]");
            else if (d->class_code == 0x02) hal_console_puts("  [Network]");
            else if (d->class_code == 0x03) hal_console_puts("  [Display]");
            else if (d->class_code == 0x04) hal_console_puts("  [Audio]");
            else if (d->class_code == 0x06) hal_console_puts("  [Bridge]");
            else if (d->class_code == 0x0C && d->subclass == 0x03)
                hal_console_puts("  [USB]");
            else if (d->class_code == 0x0C && d->subclass == 0x05)
                hal_console_puts("  [SMBus]");
            hal_console_putc('\n');
        }
    }
}

static void cmd_mem(void)
{
    uint64_t total = hal_mmu_total_ram();
    uint64_t free  = hal_mmu_free_ram();
    uint64_t used  = total - free;

    hal_console_printf("Total memory:     %u MB\n", (uint32_t)(total / (1024 * 1024)));
    hal_console_printf("In use:           %u MB\n", (uint32_t)(used / (1024 * 1024)));
    hal_console_printf("Available:        %u MB\n", (uint32_t)(free / (1024 * 1024)));
}

static void cmd_clear(void)
{
    /* Print enough newlines to scroll past visible content.
     * A proper clear would need lfb_clear() but this works universally. */
    for (int i = 0; i < 50; i++)
        hal_console_putc('\n');
}

static void cmd_drivers(void)
{
    const driver_ops_t *list[MAX_DRIVERS];
    uint32_t count = driver_list(list, MAX_DRIVERS);

    if (count == 0) {
        hal_console_puts("No hardware drivers are running yet.\n");
        return;
    }

    hal_console_printf("%u drivers active:\n", count);
    for (uint32_t i = 0; i < count; i++) {
        hal_console_puts("  ");
        hal_console_puts(list[i]->name);
        switch (list[i]->category) {
        case DRIVER_CAT_STORAGE:  hal_console_puts("  (disk/storage)");  break;
        case DRIVER_CAT_NETWORK:  hal_console_puts("  (network)");       break;
        case DRIVER_CAT_INPUT:    hal_console_puts("  (input device)");  break;
        case DRIVER_CAT_DISPLAY:  hal_console_puts("  (display)");       break;
        case DRIVER_CAT_GPU:      hal_console_puts("  (graphics)");      break;
        case DRIVER_CAT_BUS:      hal_console_puts("  (bus controller)");break;
        default:                  hal_console_puts("  (other)");         break;
        }
        hal_console_putc('\n');
    }
}

static void cmd_uptime(void)
{
    uint64_t now = hal_timer_ms();
    uint64_t up = now - g_boot_ms;
    uint32_t secs = (uint32_t)(up / 1000);
    uint32_t mins = secs / 60;
    uint32_t hrs  = mins / 60;

    secs %= 60;
    mins %= 60;

    hal_console_printf("System has been running for %u hours, %u minutes, %u seconds.\n", hrs, mins, secs);
}

static void cmd_reboot(void)
{
    hal_console_puts("Rebooting...\n");

#if defined(__x86_64__) || defined(_M_X64)
    /* x86 keyboard controller reset */
    hal_port_out8(0x64, 0xFE);
#endif

    /* Fallback: triple fault */
    for (;;)
        hal_cpu_halt();
}

static void cmd_ver(void)
{
    hal_console_puts("AlJefra OS v0.7.0\n");
    hal_console_puts("An AI-native operating system by Qatar IT (www.QatarIT.com)\n");
}

static void cmd_net(void)
{
    const driver_ops_t *net = driver_get_network();
    const dhcp_config_t *cfg = dhcp_get_config();

    hal_console_puts("Network Status\n");
    hal_console_puts("--------------\n");

    if (!net) {
        hal_console_puts("Network:      Not connected (no network driver found)\n");
        hal_console_puts("\nTip: Connect a USB Ethernet adapter or use a supported WiFi card.\n");
        return;
    }

    hal_console_puts("Network card: ");
    hal_console_puts(net->name);
    hal_console_puts("\n");

    /* Show MAC address if available */
    if (net->net_get_mac) {
        uint8_t mac[6];
        net->net_get_mac(mac);
        hal_console_printf("MAC address:  %02x:%02x:%02x:%02x:%02x:%02x\n",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (!cfg || cfg->ip == 0) {
        hal_console_puts("IP address:   Not assigned (DHCP may not have completed)\n");
        hal_console_puts("Internet:     Not connected\n");
        return;
    }

    hal_console_printf("IP address:   %u.%u.%u.%u\n",
                       (cfg->ip >> 24) & 0xFF, (cfg->ip >> 16) & 0xFF,
                       (cfg->ip >> 8) & 0xFF, cfg->ip & 0xFF);
    hal_console_printf("Gateway:      %u.%u.%u.%u\n",
                       (cfg->gateway >> 24) & 0xFF, (cfg->gateway >> 16) & 0xFF,
                       (cfg->gateway >> 8) & 0xFF, cfg->gateway & 0xFF);
    hal_console_printf("DNS server:   %u.%u.%u.%u\n",
                       (cfg->dns >> 24) & 0xFF, (cfg->dns >> 16) & 0xFF,
                       (cfg->dns >> 8) & 0xFF, cfg->dns & 0xFF);
    hal_console_printf("Subnet mask:  %u.%u.%u.%u\n",
                       (cfg->netmask >> 24) & 0xFF, (cfg->netmask >> 16) & 0xFF,
                       (cfg->netmask >> 8) & 0xFF, cfg->netmask & 0xFF);
    hal_console_puts("Internet:     Connected\n");
}
