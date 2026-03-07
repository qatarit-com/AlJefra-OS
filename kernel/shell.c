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
#include "fs.h"
#include "klog.h"
#include "ai_bootstrap.h"
#include "ai_chat.h"
#include "../hal/hal.h"
#include "../lib/string.h"
#include "../drivers/display/lfb.h"
#include "../drivers/network/intel_wifi.h"
#include "../drivers/network/usb_net.h"
#include "../drivers/input/xhci.h"

/* ── Command line buffer ── */
#define SHELL_LINE_MAX      256
#define SHELL_ARG_MAX       4
#define SHELL_READ_MAX      512
#define SHELL_WRITE_BLOCKS  1

static char g_line[SHELL_LINE_MAX];
static uint32_t g_line_pos;
static char g_read_buf[SHELL_READ_MAX + 1];
static char g_ai_buf[AI_CHAT_RESPONSE_MAX];

/* ── Boot timestamp (set when shell starts) ── */
static uint64_t g_boot_ms;

/* ── Device list (passed from kernel_main) ── */
static hal_device_t *g_devices;
static uint32_t      g_device_count;

typedef struct {
    uint32_t count;
    uint64_t total_bytes;
} shell_fs_stats_t;

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
static void cmd_ls(void);
static void cmd_cat(const char *name);
static void cmd_touch(const char *name);
static void cmd_rm(const char *name);
static void cmd_write(const char *name, const char *text);
static void cmd_df(void);
static void cmd_log(void);
static void cmd_sync(void);
static void cmd_status(void);
static void cmd_registry(void);
static void cmd_wifi(char *args);
static void cmd_setup(void);
static void cmd_diagnostics(void);
static void print_detected_network_hardware(void);
static void print_detected_usb_network_hardware(void);
static void print_bootstrap_status(void);
static void print_text_file_or_hint(const char *name, const char *missing_hint);
static void shell_print_ai_reply(const char *text);
static void shell_print_statusline(void);
static const char *shell_ai_status_short(void);
static const char *shell_ci_strstr(const char *haystack, const char *needle);
static void shell_read_line(char *buf, uint32_t max, int secret);
static int shell_load_wifi_credentials(char *ssid, uint32_t ssid_max,
                                       char *pass, uint32_t pass_max);
static void shell_save_wifi_credentials(const char *ssid, const char *pass);
static void shell_mark_setup_complete(void);

static void fs_list_print_cb(const char *name, uint64_t size, void *ctx);
static void fs_list_stats_cb(const char *name, uint64_t size, void *ctx);

/* ── Prompt ── */
static void shell_prompt(void)
{
    shell_print_statusline();
    hal_console_set_colors(LFB_COLOR_CYAN, LFB_COLOR_BLACK);
    hal_console_puts("aljefra ai> ");
    hal_console_set_colors(LFB_COLOR_WHITE, LFB_COLOR_BLACK);
}

static void shell_print_statusline(void)
{
    int fs_ok = fs_list(0, 0) >= 0;
    const driver_ops_t *net = driver_get_network();
    const dhcp_config_t *cfg = dhcp_get_config();

    hal_console_set_colors(LFB_COLOR_GRAY, LFB_COLOR_BLACK);
    hal_console_puts("[");
    hal_console_puts(fs_ok ? "fs:ok" : "fs:off");
    hal_console_puts(" | ");
    if (net && cfg && cfg->ip)
        hal_console_puts("net:on");
    else if (net)
        hal_console_puts("net:driver");
    else
        hal_console_puts("net:off");
    hal_console_puts(" | ai:");
    hal_console_puts(shell_ai_status_short());
    hal_console_puts("]\n");
    hal_console_reset_colors();
}

static const char *shell_ai_status_short(void)
{
    const char *msg = ai_bootstrap_status_message();

    if (!msg)
        return "idle";
    if (shell_ci_strstr(msg, "success") || shell_ci_strstr(msg, "registered"))
        return "ready";
    if (shell_ci_strstr(msg, "connected"))
        return "sync";
    if (shell_ci_strstr(msg, "offline"))
        return "offline";
    if (shell_ci_strstr(msg, "unreachable"))
        return "remote";
    return "wait";
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

static int count_prefix_len(const char *a, const char *b)
{
    int i = 0;
    while (b[i]) {
        if (a[i] != b[i])
            return -1;
        i++;
    }
    return i;
}

static const char *shell_ci_strstr(const char *haystack, const char *needle)
{
    uint32_t nlen = str_len(needle);
    uint32_t hlen = str_len(haystack);

    if (nlen == 0)
        return haystack;
    if (nlen > hlen)
        return (const char *)0;

    for (uint32_t i = 0; i <= hlen - nlen; i++) {
        uint32_t j = 0;
        while (j < nlen) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b)
                break;
            j++;
        }
        if (j == nlen)
            return &haystack[i];
    }

    return (const char *)0;
}

static void trim_trailing_spaces(char *s)
{
    uint32_t len = str_len(s);
    while (len > 0 && s[len - 1] == ' ') {
        s[len - 1] = '\0';
        len--;
    }
}

static int split_tokens(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_args) {
        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;

        argv[argc++] = p;

        while (*p != '\0' && *p != ' ')
            p++;

        if (*p == '\0')
            break;

        *p++ = '\0';
    }

    return argc;
}

static const char *find_write_payload(const char *line)
{
    const char *p = skip_spaces(line);

    while (*p != '\0' && *p != ' ')
        p++;
    p = skip_spaces(p);

    while (*p != '\0' && *p != ' ')
        p++;
    p = skip_spaces(p);

    return p;
}

static void shell_read_line(char *buf, uint32_t max, int secret)
{
    uint32_t pos = 0;

    if (max == 0)
        return;

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            hal_console_putc('\n');
            break;
        }

        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                pos--;
                hal_console_putc('\b');
                hal_console_putc(' ');
                hal_console_putc('\b');
            }
            continue;
        }

        if (c >= ' ' && pos + 1 < max) {
            buf[pos++] = c;
            hal_console_putc(secret ? '*' : c);
        }
    }

    buf[pos] = '\0';
}

static int shell_load_wifi_credentials(char *ssid, uint32_t ssid_max,
                                       char *pass, uint32_t pass_max)
{
    char buf[256];
    int fd = fs_open("wifi.conf");
    int64_t rd;
    const char *cur;

    if (fd < 0)
        return -1;

    rd = fs_read(fd, buf, 0, sizeof(buf) - 1);
    fs_close(fd);
    if (rd <= 0)
        return -1;

    buf[rd] = '\0';
    ssid[0] = '\0';
    pass[0] = '\0';

    cur = buf;
    while (*cur) {
        uint32_t n = 0;
        char line[96];

        while (cur[n] && cur[n] != '\n' && cur[n] != '\r' && n + 1 < sizeof(line)) {
            line[n] = cur[n];
            n++;
        }
        line[n] = '\0';

        if (count_prefix_len(line, "ssid=") > 0)
            str_copy(ssid, line + 5, ssid_max);
        else if (count_prefix_len(line, "passphrase=") > 0)
            str_copy(pass, line + 11, pass_max);
        else if (count_prefix_len(line, "password=") > 0)
            str_copy(pass, line + 9, pass_max);

        cur += n;
        while (*cur == '\n' || *cur == '\r')
            cur++;
    }

    return (ssid[0] && pass[0]) ? 0 : -1;
}

static void shell_save_wifi_credentials(const char *ssid, const char *pass)
{
    int fd;
    char buf[160];

    if (!ssid || !pass)
        return;

    str_copy(buf, "ssid=", sizeof(buf));
    str_copy(buf + str_len(buf), ssid, sizeof(buf) - str_len(buf));
    str_copy(buf + str_len(buf), "\npassphrase=", sizeof(buf) - str_len(buf));
    str_copy(buf + str_len(buf), pass, sizeof(buf) - str_len(buf));
    str_copy(buf + str_len(buf), "\n", sizeof(buf) - str_len(buf));

    fd = fs_create("wifi.conf", 1);
    if (fd < 0)
        return;

    fs_write(fd, buf, 0, str_len(buf));
    fs_close(fd);
    fs_sync();
}

static void shell_mark_setup_complete(void)
{
    int fd = fs_create("setup-complete.flag", 1);
    if (fd < 0)
        return;
    fs_write(fd, "done\n", 0, 5);
    fs_close(fd);
    fs_sync();
}

/* ── Execute a command ── */
static void shell_exec(char *cmdline)
{
    char *argv[SHELL_ARG_MAX];
    int argc;

    cmdline = (char *)skip_spaces(cmdline);
    trim_trailing_spaces(cmdline);

    if (*cmdline == '\0')
        return;

    argc = split_tokens(cmdline, argv, SHELL_ARG_MAX);
    if (argc == 0)
        return;

    if (str_eq(argv[0], "help") || str_eq(argv[0], "?"))
        cmd_help();
    else if (str_eq(argv[0], "info") || str_eq(argv[0], "uname"))
        cmd_info();
    else if (str_eq(argv[0], "pci") || str_eq(argv[0], "lspci"))
        cmd_pci();
    else if (str_eq(argv[0], "mem") || str_eq(argv[0], "free"))
        cmd_mem();
    else if (str_eq(argv[0], "clear") || str_eq(argv[0], "cls"))
        cmd_clear();
    else if (str_eq(argv[0], "drivers") || str_eq(argv[0], "lsmod"))
        cmd_drivers();
    else if (str_eq(argv[0], "uptime"))
        cmd_uptime();
    else if (str_eq(argv[0], "reboot"))
        cmd_reboot();
    else if (str_eq(argv[0], "version") || str_eq(argv[0], "ver"))
        cmd_ver();
    else if (str_eq(argv[0], "net") || str_eq(argv[0], "network") || str_eq(argv[0], "ip"))
        cmd_net();
    else if (str_eq(argv[0], "status"))
        cmd_status();
    else if (str_eq(argv[0], "registry") || str_eq(argv[0], "sync-status") ||
             str_eq(argv[0], "assistant"))
        cmd_registry();
    else if (str_eq(argv[0], "wifi"))
        cmd_wifi(argc > 1 ? argv[1] : (char *)"");
    else if (str_eq(argv[0], "setup") || str_eq(argv[0], "wizard"))
        cmd_setup();
    else if (str_eq(argv[0], "diagnostics") || str_eq(argv[0], "diag"))
        cmd_diagnostics();
    else if (str_eq(argv[0], "ls") || str_eq(argv[0], "dir"))
        cmd_ls();
    else if (str_eq(argv[0], "cat") || str_eq(argv[0], "read")) {
        if (argc < 2)
            hal_console_puts("Usage: cat <file>\n");
        else
            cmd_cat(argv[1]);
    } else if (str_eq(argv[0], "touch") || str_eq(argv[0], "create")) {
        if (argc < 2)
            hal_console_puts("Usage: touch <file>\n");
        else
            cmd_touch(argv[1]);
    } else if (str_eq(argv[0], "rm") || str_eq(argv[0], "delete")) {
        if (argc < 2)
            hal_console_puts("Usage: rm <file>\n");
        else
            cmd_rm(argv[1]);
    } else if (str_eq(argv[0], "write")) {
        const char *payload = find_write_payload(cmdline);
        if (argc < 3 || *payload == '\0')
            hal_console_puts("Usage: write <file> <text>\n");
        else
            cmd_write(argv[1], payload);
    } else if (str_eq(argv[0], "df") || str_eq(argv[0], "files"))
        cmd_df();
    else if (str_eq(argv[0], "log") || str_eq(argv[0], "dmesg"))
        cmd_log();
    else if (str_eq(argv[0], "sync"))
        cmd_sync();
    else {
        int ai_len = ai_chat_process(cmdline, g_ai_buf, sizeof(g_ai_buf));
        if (ai_len > 0 && g_ai_buf[0] != '\0') {
            shell_print_ai_reply(g_ai_buf);
        } else {
            hal_console_set_colors(LFB_COLOR_YELLOW, LFB_COLOR_BLACK);
            hal_console_puts("I could not understand that yet.\n");
            hal_console_puts("Type 'help' for direct commands or ask in plain language.\n");
            hal_console_reset_colors();
        }
    }
}

/* ── Main shell loop ── */
void shell_run(void)
{
    g_boot_ms = hal_timer_ms();
    g_line_pos = 0;

    hal_console_puts("\n");
    hal_console_puts("Welcome! AlJefra OS is ready.\n");
    hal_console_puts("Ask naturally for help, networking, files, or system actions.\n");
    hal_console_puts("Direct commands still work: help, status, net, wifi, setup, diagnostics.\n");
    {
        int fd = fs_open("setup-complete.flag");
        if (fd < 0) {
            hal_console_puts("First-time setup is available. Type 'setup' for guided networking.\n");
        } else {
            fs_close(fd);
        }
    }
    hal_console_puts("\n");
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
            if (g_line_pos > 0) {
                g_line_pos--;
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
    hal_console_puts("Commands:\n\n");
    hal_console_puts("  help           - Show this guide\n");
    hal_console_puts("  status         - High-level system summary\n");
    hal_console_puts("  info           - CPU, RAM, architecture\n");
    hal_console_puts("  net            - Network status and IP address\n");
    hal_console_puts("  wifi           - Save or use Wi-Fi credentials\n");
    hal_console_puts("  setup          - Guided first-boot network setup\n");
    hal_console_puts("  diagnostics    - Show saved boot diagnostics\n");
    hal_console_puts("  registry       - AI registration and sync status\n");
    hal_console_puts("  pci            - Enumerate detected hardware devices\n");
    hal_console_puts("  mem            - Memory usage\n");
    hal_console_puts("  drivers        - Loaded hardware drivers\n");
    hal_console_puts("  ls             - List BMFS files\n");
    hal_console_puts("  cat <file>     - Print a file\n");
    hal_console_puts("  touch <file>   - Create a new file\n");
    hal_console_puts("  write <f> <t>  - Overwrite file with text\n");
    hal_console_puts("  rm <file>      - Delete a file\n");
    hal_console_puts("  df             - Filesystem summary\n");
    hal_console_puts("  log            - Dump kernel log ring buffer\n");
    hal_console_puts("  sync           - Flush filesystem + log to disk\n");
    hal_console_puts("  uptime         - Time since shell start\n");
    hal_console_puts("  clear          - Clear the screen\n");
    hal_console_puts("  version        - AlJefra OS version info\n");
    hal_console_puts("  reboot         - Restart the computer\n");
}

static void cmd_info(void)
{
    hal_cpu_info_t cpu;
    hal_cpu_get_info(&cpu);

    hal_console_puts("AlJefra OS v0.7.11\n");
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
            if (d->class_code == 0x01) hal_console_puts("  [Storage]");
            else if (d->class_code == 0x02) hal_console_puts("  [Network]");
            else if (d->class_code == 0x03) hal_console_puts("  [Display]");
            else if (d->class_code == 0x04) hal_console_puts("  [Audio]");
            else if (d->class_code == 0x06) hal_console_puts("  [Bridge]");
            else if (d->class_code == 0x0C && d->subclass == 0x03) hal_console_puts("  [USB]");
            hal_console_putc('\n');
        }
    }
}

static void cmd_mem(void)
{
    uint64_t total = hal_mmu_total_ram();
    uint64_t free = hal_mmu_free_ram();
    uint64_t used = total - free;

    hal_console_printf("Total memory:     %u MB\n", (uint32_t)(total / (1024 * 1024)));
    hal_console_printf("In use:           %u MB\n", (uint32_t)(used / (1024 * 1024)));
    hal_console_printf("Available:        %u MB\n", (uint32_t)(free / (1024 * 1024)));
}

static void cmd_clear(void)
{
    hal_console_clear();
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
        case DRIVER_CAT_STORAGE: hal_console_puts("  (disk/storage)"); break;
        case DRIVER_CAT_NETWORK: hal_console_puts("  (network)"); break;
        case DRIVER_CAT_INPUT: hal_console_puts("  (input device)"); break;
        case DRIVER_CAT_DISPLAY: hal_console_puts("  (display)"); break;
        case DRIVER_CAT_GPU: hal_console_puts("  (graphics)"); break;
        case DRIVER_CAT_BUS: hal_console_puts("  (bus controller)"); break;
        default: hal_console_puts("  (other)"); break;
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
    uint32_t hrs = mins / 60;

    secs %= 60;
    mins %= 60;

    hal_console_printf("System has been running for %u hours, %u minutes, %u seconds.\n", hrs, mins, secs);
}

static void cmd_reboot(void)
{
    hal_console_puts("Rebooting...\n");
    klog(KLOG_WARN, "shell: reboot requested");
    klog_flush();

#if defined(__x86_64__) || defined(_M_X64)
    hal_port_out8(0x64, 0xFE);
#endif

    for (;;)
        hal_cpu_halt();
}

static void cmd_ver(void)
{
    hal_console_puts("AlJefra OS v0.7.11\n");
    hal_console_puts("AI-native operating system project by Qatar IT\n");
}

static void cmd_net(void)
{
    const driver_ops_t *net = driver_get_network();
    const dhcp_config_t *cfg = dhcp_get_config();

    hal_console_puts("Network Status\n");
    hal_console_puts("--------------\n");
    hal_console_puts("DHCP:         ");
    hal_console_puts(dhcp_last_status_message());
    hal_console_putc('\n');

    if (!net) {
        hal_console_puts("Network:      No active network driver\n");
        print_detected_network_hardware();
        print_detected_usb_network_hardware();
        return;
    }

    hal_console_puts("Network card: ");
    hal_console_puts(net->name);
    hal_console_puts("\n");

    if (net->net_get_mac) {
        uint8_t mac[6];
        net->net_get_mac(mac);
        hal_console_printf("MAC address:  %02x:%02x:%02x:%02x:%02x:%02x\n",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (!cfg || cfg->ip == 0) {
        hal_console_puts("IP address:   Not assigned (DHCP may not have completed)\n");
        hal_console_puts("Internet:     Not connected\n");
        print_detected_network_hardware();
        print_detected_usb_network_hardware();
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
    hal_console_puts("Internet:     Connected\n");
    print_detected_usb_network_hardware();
}

static void cmd_wifi(char *args)
{
    char ssid[64];
    char pass[96];
    dhcp_config_t cfg;

    if (args && str_eq(args, "status")) {
        cmd_net();
        return;
    }

    if (!driver_find_by_name("intel_wifi")) {
        hal_console_puts("Wi-Fi setup is only available for supported Intel Wi-Fi right now.\n");
        hal_console_puts("If your internal card is unsupported, use USB Ethernet and run 'net'.\n");
        return;
    }

    if (args && str_eq(args, "connect")) {
        if (shell_load_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) < 0) {
            hal_console_puts("No saved wifi.conf found. Run 'wifi' or 'setup' first.\n");
            return;
        }

        hal_console_printf("Connecting to Wi-Fi '%s'...\n", ssid);
        if (intel_wifi_connect_saved(ssid, pass) != HAL_OK) {
            hal_console_puts("Wi-Fi activation failed.\n");
            return;
        }

        driver_set_active_network("intel_wifi");
        if (dhcp_init(&cfg) == HAL_OK)
            hal_console_puts("Wi-Fi connected and DHCP completed.\n");
        else
            hal_console_puts("Wi-Fi driver is active, but DHCP did not complete.\n");
        return;
    }

    hal_console_puts("Wi-Fi setup\n");
    hal_console_puts("---------\n");
    hal_console_puts("SSID: ");
    shell_read_line(ssid, sizeof(ssid), 0);
    hal_console_puts("Passphrase: ");
    shell_read_line(pass, sizeof(pass), 1);

    if (ssid[0] == '\0' || pass[0] == '\0') {
        hal_console_puts("Wi-Fi setup cancelled because the SSID or passphrase was empty.\n");
        return;
    }

    shell_save_wifi_credentials(ssid, pass);
    hal_console_puts("Saved wifi.conf. Trying the connection now...\n");
    cmd_wifi("connect");
}

static void cmd_setup(void)
{
    hal_console_puts("Setup wizard\n");
    hal_console_puts("------------\n");
    hal_console_puts("1. We will try to get networking working.\n");
    hal_console_puts("2. If your laptop uses supported Intel Wi-Fi, we can save credentials now.\n");
    hal_console_puts("3. If not, plug your USB Ethernet adapter and use 'net' to inspect it.\n\n");

    if (driver_get_network() && dhcp_get_config() && dhcp_get_config()->ip) {
        hal_console_puts("Networking already looks healthy.\n");
    } else if (driver_find_by_name("intel_wifi")) {
        cmd_wifi((char *)"");
    } else if (usb_net_is_ready()) {
        hal_console_puts("USB Ethernet is detected. Run 'net' to see whether DHCP completed.\n");
    } else {
        hal_console_puts("No supported Wi-Fi driver is active yet. Plug USB Ethernet, then run 'net'.\n");
    }

    shell_mark_setup_complete();
    hal_console_puts("Setup note saved. You can run 'diagnostics' any time.\n");
}

static void cmd_diagnostics(void)
{
    hal_console_puts("Diagnostics\n");
    hal_console_puts("-----------\n");
    print_text_file_or_hint("boot-diagnostics.txt",
                            "No boot diagnostics file has been saved yet.\n");
    hal_console_puts("Current DHCP status: ");
    hal_console_puts(dhcp_last_status_message());
    hal_console_putc('\n');
}

static void print_detected_network_hardware(void)
{
    uint32_t found = 0;

    if (!g_devices || g_device_count == 0) {
        hal_console_puts("Detected NICs: hardware scan unavailable\n");
        return;
    }

    for (uint32_t i = 0; i < g_device_count; i++) {
        hal_device_t *d = &g_devices[i];
        if (d->class_code != 0x02)
            continue;

        if (found == 0)
            hal_console_puts("Detected NICs:\n");

        hal_console_printf("  %02x:%02x.%x  %04x:%04x  class %02x:%02x\n",
                           d->bus, d->dev, d->func,
                           d->vendor_id, d->device_id,
                           d->class_code, d->subclass);
        found++;
    }

    if (found == 0)
        hal_console_puts("Detected NICs: none\n");
}

static void print_detected_usb_network_hardware(void)
{
    xhci_controller_t *hc = xhci_get_controller();
    uint32_t found = 0;

    if (usb_net_is_ready()) {
        hal_console_printf("USB NIC:      slot %u  %04x:%04x  ready\n",
                           usb_net_slot_id(), usb_net_vendor_id(),
                           usb_net_product_id());
        found = 1;
    }

    if (!hc) {
        if (!found) {
            hal_console_puts("USB NICs:     xHCI unavailable\n");
            if (g_devices && g_device_count) {
                for (uint32_t i = 0; i < g_device_count; i++) {
                    hal_device_t *d = &g_devices[i];
                    if (d->class_code == 0x0C && d->subclass == 0x03) {
                        hal_console_printf("USB ctrl:     %02x:%02x.%x  %04x:%04x  prog_if %02x\n",
                                           d->bus, d->dev, d->func,
                                           d->vendor_id, d->device_id,
                                           d->prog_if);
                        found = 1;
                    }
                }
            }
        }
        return;
    }

    for (uint8_t i = 0; i < hc->max_slots; i++) {
        xhci_slot_t *slot = &hc->slots[i];
        usb_device_desc_t desc;
        usb_device_desc_t *dd;
        int maybe_net;

        if (!slot->active)
            continue;

        dd = &slot->dev_desc;
        if (dd->idVendor == 0 && dd->idProduct == 0) {
            if (xhci_get_device_desc(hc, slot->slot_id, &desc) != HAL_OK)
                continue;
            dd = &desc;
        }

        if (dd->idVendor == 0 && dd->idProduct == 0)
            continue;

        maybe_net = (dd->idVendor == 0x0B95 || dd->idVendor == 0x0BDA ||
                     dd->bDeviceClass == USB_CLASS_CDC ||
                     dd->bDeviceClass == USB_CLASS_PER_IFACE);
        if (!maybe_net)
            continue;

        hal_console_printf("USB probe:    slot %u  %04x:%04x class %02x sub %02x\n",
                           slot->slot_id, dd->idVendor, dd->idProduct,
                           dd->bDeviceClass, dd->bDeviceSubClass);
        found = 1;
    }

    if (!found)
        hal_console_puts("USB NICs:     none detected on xHCI slots\n");
}

static void shell_print_ai_reply(const char *text)
{
    hal_console_set_colors(LFB_COLOR_GREEN, LFB_COLOR_BLACK);
    hal_console_puts("aljefra> ");
    hal_console_set_colors(LFB_COLOR_GRAY, LFB_COLOR_BLACK);
    hal_console_puts(text);
    hal_console_reset_colors();
    if (text[str_len(text) - 1] != '\n')
        hal_console_putc('\n');
}

static void fs_list_print_cb(const char *name, uint64_t size, void *ctx)
{
    (void)ctx;
    hal_console_puts("  ");
    hal_console_puts(name);
    hal_console_puts("  ");
    hal_console_printf("%u bytes\n", (uint32_t)size);
}

static void fs_list_stats_cb(const char *name, uint64_t size, void *ctx)
{
    shell_fs_stats_t *stats = (shell_fs_stats_t *)ctx;
    (void)name;
    stats->count++;
    stats->total_bytes += size;
}

static void cmd_ls(void)
{
    int count = fs_list(fs_list_print_cb, 0);
    if (count < 0) {
        hal_console_puts("Filesystem not available.\n");
        return;
    }
    if (count == 0)
        hal_console_puts("No files found.\n");
}

static void cmd_cat(const char *name)
{
    int fd = fs_open(name);
    if (fd < 0) {
        hal_console_puts("File not found: ");
        hal_console_puts(name);
        hal_console_putc('\n');
        return;
    }

    uint64_t size = fs_size(fd);
    uint64_t to_read = size;
    if (to_read > SHELL_READ_MAX)
        to_read = SHELL_READ_MAX;

    int64_t rd = fs_read(fd, g_read_buf, 0, to_read);
    fs_close(fd);
    if (rd < 0) {
        hal_console_puts("Read failed.\n");
        return;
    }

    g_read_buf[rd] = '\0';
    hal_console_printf("File: %s (%u bytes)\n", name, (uint32_t)size);
    hal_console_puts("--------------------------------\n");
    hal_console_puts(g_read_buf);
    if (to_read < size)
        hal_console_puts("\n... truncated ...");
    hal_console_putc('\n');
}

static void cmd_touch(const char *name)
{
    if (fs_create(name, SHELL_WRITE_BLOCKS) == 0) {
        hal_console_puts("Created file: ");
        hal_console_puts(name);
        hal_console_putc('\n');
        klog(KLOG_INFO, "shell: created file %s", name);
    } else {
        hal_console_puts("Failed to create file: ");
        hal_console_puts(name);
        hal_console_putc('\n');
    }
}

static void cmd_rm(const char *name)
{
    if (fs_delete(name) == 0) {
        hal_console_puts("Deleted file: ");
        hal_console_puts(name);
        hal_console_putc('\n');
        klog(KLOG_WARN, "shell: deleted file %s", name);
    } else {
        hal_console_puts("Failed to delete file: ");
        hal_console_puts(name);
        hal_console_putc('\n');
    }
}

static void cmd_write(const char *name, const char *text)
{
    int fd = fs_open(name);
    int created = 0;
    uint32_t len = str_len(text);

    if (fd < 0) {
        if (fs_create(name, SHELL_WRITE_BLOCKS) != 0) {
            hal_console_puts("Failed to create file for write: ");
            hal_console_puts(name);
            hal_console_putc('\n');
            return;
        }
        created = 1;
        fd = fs_open(name);
    }

    if (fd < 0) {
        hal_console_puts("Could not open file after create.\n");
        return;
    }

    if (fs_write(fd, text, 0, len) < 0) {
        hal_console_puts("Write failed.\n");
        fs_close(fd);
        return;
    }

    fs_close(fd);
    hal_console_printf("Wrote %u bytes to %s%s\n", len, name, created ? " (new file)" : "");
    klog(KLOG_INFO, "shell: wrote %u bytes to %s", len, name);
}

static void cmd_df(void)
{
    shell_fs_stats_t stats;
    int count;

    stats.count = 0;
    stats.total_bytes = 0;

    count = fs_list(fs_list_stats_cb, &stats);
    if (count < 0) {
        hal_console_puts("Filesystem not available.\n");
        return;
    }

    hal_console_printf("Files:          %u\n", stats.count);
    hal_console_printf("Used bytes:     %u\n", (uint32_t)stats.total_bytes);
    hal_console_printf("Approx. used:   %u KB\n", (uint32_t)(stats.total_bytes / 1024));
}

static void cmd_log(void)
{
    klog_dump();
}

static void cmd_sync(void)
{
    int fs_rc = fs_sync();
    int log_rc = klog_flush();

    if (fs_rc == 0)
        hal_console_puts("Filesystem metadata flushed.\n");
    else
        hal_console_puts("Filesystem flush skipped or failed.\n");

    if (log_rc == 0)
        hal_console_puts("Kernel log flushed.\n");
    else
        hal_console_puts("Kernel log flush skipped or failed.\n");
}

static void cmd_status(void)
{
    const driver_ops_t *list[MAX_DRIVERS];
    uint32_t count = driver_list(list, MAX_DRIVERS);
    const dhcp_config_t *cfg = dhcp_get_config();

    hal_console_puts("System Status\n");
    hal_console_puts("-------------\n");
    cmd_info();
    hal_console_printf("Hardware devices: %u\n", g_device_count);
    hal_console_printf("Loaded drivers:   %u\n", count);
    hal_console_puts("Filesystem:       ");
    if (fs_list(0, 0) >= 0)
        hal_console_puts("BMFS mounted\n");
    else
        hal_console_puts("Unavailable\n");
    hal_console_puts("Network:          ");
    if (driver_get_network()) {
        if (cfg && cfg->ip)
            hal_console_puts("Connected\n");
        else
            hal_console_puts("Driver loaded, waiting for DHCP\n");
    } else
        hal_console_puts("No driver\n");
    hal_console_puts("DHCP:             ");
    hal_console_puts(dhcp_last_status_message());
    hal_console_putc('\n');
    hal_console_puts("AI sync:          ");
    print_bootstrap_status();
}

static void cmd_registry(void)
{
    hal_console_puts("AI Registration\n");
    hal_console_puts("---------------\n");
    hal_console_puts("Status: ");
    print_bootstrap_status();
    hal_console_puts("\nHardware Profile\n");
    hal_console_puts("----------------\n");
    print_text_file_or_hint("hardware-profile.txt",
                            "Hardware profile is not saved yet.\n");
    hal_console_puts("\nMarketplace Sync\n");
    hal_console_puts("----------------\n");
    print_text_file_or_hint("marketplace-sync.txt",
                            "Marketplace sync has not completed yet.\n");
}

static void print_bootstrap_status(void)
{
    hal_console_puts(ai_bootstrap_status_message());
    hal_console_putc('\n');
}

static void print_text_file_or_hint(const char *name, const char *missing_hint)
{
    int fd;
    int64_t rd;
    char buf[640];

    fd = fs_open(name);
    if (fd < 0) {
        hal_console_puts(missing_hint);
        return;
    }

    rd = fs_read(fd, buf, 0, sizeof(buf) - 1);
    fs_close(fd);
    if (rd <= 0) {
        hal_console_puts(missing_hint);
        return;
    }

    buf[rd] = '\0';
    hal_console_puts(buf);
    if (buf[rd - 1] != '\n')
        hal_console_putc('\n');
}
