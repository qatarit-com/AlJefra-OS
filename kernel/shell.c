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
#include "../hal/hal.h"
#include "../lib/string.h"

/* ── Command line buffer ── */
#define SHELL_LINE_MAX      256
#define SHELL_ARG_MAX       4
#define SHELL_READ_MAX      512
#define SHELL_WRITE_BLOCKS  1

static char g_line[SHELL_LINE_MAX];
static uint32_t g_line_pos;
static char g_read_buf[SHELL_READ_MAX + 1];

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
static void print_detected_network_hardware(void);

static void fs_list_print_cb(const char *name, uint64_t size, void *ctx);
static void fs_list_stats_cb(const char *name, uint64_t size, void *ctx);

/* ── Prompt ── */
static void shell_prompt(void)
{
    hal_console_puts("aljefra> ");
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
        s++;
    return s;
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
        hal_console_puts("Unknown command: ");
        hal_console_puts(argv[0]);
        hal_console_puts("\nType 'help' to see available commands.\n");
    }
}

/* ── Main shell loop ── */
void shell_run(void)
{
    g_boot_ms = hal_timer_ms();
    g_line_pos = 0;

    hal_console_puts("\n");
    hal_console_puts("Welcome! AlJefra OS is ready.\n");
    hal_console_puts("Type 'help' for commands, 'status' for a system summary, or 'ls' to inspect files.\n\n");
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

    hal_console_puts("AlJefra OS v0.7.4\n");
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
    hal_console_puts("AlJefra OS v0.7.4\n");
    hal_console_puts("AI-native operating system project by Qatar IT\n");
}

static void cmd_net(void)
{
    const driver_ops_t *net = driver_get_network();
    const dhcp_config_t *cfg = dhcp_get_config();

    hal_console_puts("Network Status\n");
    hal_console_puts("--------------\n");

    if (!net) {
        hal_console_puts("Network:      No active network driver\n");
        print_detected_network_hardware();
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
    if (driver_get_network())
        hal_console_puts("Driver loaded\n");
    else
        hal_console_puts("No driver\n");
}
