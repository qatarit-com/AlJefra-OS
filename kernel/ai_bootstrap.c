/* SPDX-License-Identifier: MIT */
/* AlJefra OS — AI Bootstrap Implementation
 *
 * Boot sequence: Hardware detect → Network → Marketplace → Drivers → Ready
 *
 * The AI bootstrap is the core of the universal boot concept:
 * we only need enough drivers to get network connectivity,
 * then AI handles the rest by downloading appropriate drivers.
 */

#include "ai_bootstrap.h"
#include "driver_loader.h"
#include "../hal/hal.h"
#include "../store/verify.h"
#include "../lib/string.h"
#include "fs.h"

/* Network/marketplace modules (from net/ and ai/) */
extern hal_status_t dhcp_discover(uint32_t *ip, uint32_t *gateway, uint32_t *dns);
extern void tcp_init(uint32_t local_ip, uint32_t gateway, uint32_t netmask);
#include "../ai/marketplace.h"

/* AlJefra Store Ed25519 public key (32 bytes).
 * In development mode this is all-zeros, and signature verification is skipped.
 * For production, replace with the real store signing key. */
static const uint8_t ALJEFRA_STORE_PUBKEY[32] = {0};

static bootstrap_state_t g_state = BOOTSTRAP_INIT;
static char g_status_message[160] = "Waiting for network and marketplace";

static void append_u16_hex(char **p, char *end, uint16_t value)
{
    static const char hex[] = "0123456789abcdef";
    if (*p >= end)
        return;
    *(*p)++ = hex[(value >> 12) & 0xF];
    if (*p >= end)
        return;
    *(*p)++ = hex[(value >> 8) & 0xF];
    if (*p >= end)
        return;
    *(*p)++ = hex[(value >> 4) & 0xF];
    if (*p >= end)
        return;
    *(*p)++ = hex[value & 0xF];
}

static void set_status(bootstrap_state_t state, const char *message)
{
    g_state = state;
    if (!message)
        return;

    str_copy(g_status_message, message, sizeof(g_status_message));
}

static void persist_text_file(const char *name, const char *text)
{
    int fd;

    if (!name || !text)
        return;

    fd = fs_open(name);
    if (fd < 0) {
        if (fs_create(name, 2) < 0)
            return;
        fd = fs_open(name);
        if (fd < 0)
            return;
    }

    fs_write(fd, text, 0, str_len(text));
    fs_close(fd);
}

static int load_desired_apps(char *out, uint32_t out_max)
{
    int fd = fs_open("desired-apps.conf");
    if (fd < 0) {
        out[0] = '\0';
        return -1;
    }

    int64_t rd = fs_read(fd, out, 0, out_max - 1);
    fs_close(fd);
    if (rd <= 0) {
        out[0] = '\0';
        return -1;
    }

    out[rd] = '\0';
    return 0;
}

static void persist_sync_report(const char *report)
{
    if (!report || !report[0])
        return;

    persist_text_file("marketplace-sync.txt", report);
    fs_sync();
}

static void persist_hardware_profile(const hardware_manifest_t *manifest)
{
    char buf[4096];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;

    if (!manifest)
        return;

    #define APPENDC(s) do { \
        const char *_s = (s); \
        while (*_s && p < end) *p++ = *_s++; \
    } while (0)
    #define APPENDU(v) do { \
        char _n[16]; \
        int _i = 0; \
        uint32_t _v = (uint32_t)(v); \
        if (_v == 0) { \
            if (p < end) *p++ = '0'; \
        } else { \
            while (_v > 0 && _i < (int)sizeof(_n)) { \
                _n[_i++] = '0' + (_v % 10); \
                _v /= 10; \
            } \
            while (_i > 0 && p < end) *p++ = _n[--_i]; \
        } \
    } while (0)

    APPENDC("AlJefra OS Hardware Profile\n");
    APPENDC("==========================\n");
    APPENDC("Architecture: ");
    switch (manifest->arch) {
    case HAL_ARCH_X86_64: APPENDC("x86_64"); break;
    case HAL_ARCH_AARCH64: APPENDC("aarch64"); break;
    case HAL_ARCH_RISCV64: APPENDC("riscv64"); break;
    }
    APPENDC("\nCPU Vendor: ");
    APPENDC(manifest->cpu_vendor);
    APPENDC("\nCPU Model: ");
    APPENDC(manifest->cpu_model);
    APPENDC("\nMemory MB: ");
    APPENDU((uint32_t)(manifest->ram_bytes / (1024 * 1024)));
    APPENDC("\nDevices:\n");

    for (uint32_t i = 0; i < manifest->entry_count; i++) {
        const manifest_entry_t *e = &manifest->entries[i];
        APPENDC("  ");
        append_u16_hex(&p, end, e->vendor_id);
        APPENDC(":");
        append_u16_hex(&p, end, e->device_id);
        APPENDC(" class ");
        APPENDU(e->class_code);
        APPENDC(":");
        APPENDU(e->subclass);
        APPENDC(" driver=");
        APPENDC(e->has_driver ? "yes" : "no");
        APPENDC("\n");
    }

    *p = '\0';
    persist_text_file("hardware-profile.txt", buf);

    #undef APPENDC
    #undef APPENDU
}

/* ── Helpers ── */

/* Check if a device already has a loaded driver */
static int device_has_driver(hal_device_t *dev)
{
    /* Check if any loaded driver handles this vendor:device */
    const driver_ops_t *net = driver_get_network();
    const driver_ops_t *stor = driver_get_storage();
    const driver_ops_t *inp = driver_get_input();

    /* If we have drivers in the major categories, the device is likely covered */
    (void)dev; (void)net; (void)stor; (void)inp;

    /* For now, check by PCI class if a driver category is filled */
    switch (dev->class_code) {
    case 0x01: return driver_get_storage() != NULL;
    case 0x02: return driver_get_network() != NULL;
    case 0x03: return driver_find(DRIVER_CAT_DISPLAY) != NULL;
    case 0x0C: return driver_get_input() != NULL;
    default: return 0;
    }
}

/* ── Build hardware manifest ── */

void ai_bootstrap_build_manifest(hal_device_t *devices, uint32_t count,
                                  hardware_manifest_t *manifest)
{
    manifest->arch = hal_arch();
    manifest->ram_bytes = hal_mmu_total_ram();

    hal_cpu_info_t cpu;
    hal_cpu_get_info(&cpu);

    /* Copy CPU strings */
    for (int i = 0; i < 16 && cpu.vendor[i]; i++)
        manifest->cpu_vendor[i] = cpu.vendor[i];
    for (int i = 0; i < 48 && cpu.model[i]; i++)
        manifest->cpu_model[i] = cpu.model[i];

    manifest->entry_count = 0;
    for (uint32_t i = 0; i < count && manifest->entry_count < HAL_BUS_MAX_DEVICES; i++) {
        manifest_entry_t *e = &manifest->entries[manifest->entry_count];
        e->vendor_id = devices[i].vendor_id;
        e->device_id = devices[i].device_id;
        e->class_code = devices[i].class_code;
        e->subclass = devices[i].subclass;
        e->has_driver = device_has_driver(&devices[i]) ? 1 : 0;
        e->reserved = 0;
        manifest->entry_count++;
    }
}

/* ── Main bootstrap sequence ── */

hal_status_t ai_bootstrap(hal_device_t *devices, uint32_t count)
{
    hal_status_t rc;

    hal_console_puts("  Checking if any hardware needs additional drivers...\n");

    /* Step 0: Set trusted public key for driver signature verification.
     * If the key is all-zeros (dev mode), verification is skipped. */
    int key_nonzero = 0;
    for (int i = 0; i < 32; i++) {
        if (ALJEFRA_STORE_PUBKEY[i]) { key_nonzero = 1; break; }
    }
    if (key_nonzero)
        ajdrv_set_trusted_key(ALJEFRA_STORE_PUBKEY);

    /* Step 1: Build hardware manifest */
    hardware_manifest_t manifest;
    for (uint64_t i = 0; i < sizeof(manifest); i++)
        ((uint8_t *)&manifest)[i] = 0;

    ai_bootstrap_build_manifest(devices, count, &manifest);

    hal_console_printf("  Detected %u devices, %u MB RAM\n",
                       manifest.entry_count,
                       (uint32_t)(manifest.ram_bytes / (1024 * 1024)));

    /* Count devices needing drivers */
    uint32_t need_drivers = 0;
    for (uint32_t i = 0; i < manifest.entry_count; i++) {
        if (!manifest.entries[i].has_driver)
            need_drivers++;
    }

    persist_hardware_profile(&manifest);

    if (need_drivers == 0)
        hal_console_puts("  All hardware is supported — registering this machine anyway.\n");
    else
        hal_console_printf("  %u devices need drivers — will try to download them.\n", need_drivers);

    /* Step 2: Check network availability */
    const driver_ops_t *net = driver_get_network();
    if (!net) {
        hal_console_puts("  No network connection available.\n");
        hal_console_puts("  Using built-in drivers only. Connect a network adapter for more.\n");
        persist_sync_report("Offline: machine not yet registered. Connect to the network and reboot to sync drivers and apps.\n");
        set_status(BOOTSTRAP_FAILED, "Offline: connect to the network to register this machine");
        return HAL_NO_DEVICE;
    }

    /* Step 3: DHCP to get network configuration */
    hal_console_puts("  Getting network address (DHCP)...\n");
    uint32_t ip = 0, gateway = 0, dns = 0;
    rc = dhcp_discover(&ip, &gateway, &dns);
    if (rc != HAL_OK) {
        hal_console_puts("  Could not get address automatically, using default.\n");
        /* Fall back to static IP if configured */
        ip = 0x0A000002;       /* 10.0.0.2 */
        gateway = 0x0A000001;  /* 10.0.0.1 */
        dns = 0x08080808;      /* 8.8.8.8 */
    }

    hal_console_printf("  Your IP address: %u.%u.%u.%u\n",
                       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                       (ip >> 8) & 0xFF, ip & 0xFF);

    /* Initialize TCP stack and marketplace with DHCP results */
    tcp_init(ip, gateway, 0xFFFFFF00); /* /24 netmask */
    marketplace_set_gateway(gateway);

    set_status(BOOTSTRAP_NET_UP, "Network connected; preparing marketplace sync");

    /* Step 4: Connect to marketplace */
    hal_console_puts("  Connecting to AlJefra Store...\n");
    rc = marketplace_connect();
    if (rc != HAL_OK) {
        hal_console_puts("  Could not reach AlJefra Store. Continuing with built-in drivers.\n");
        persist_sync_report("Online network available, but the marketplace could not be reached.\n");
        set_status(BOOTSTRAP_FAILED, "Network is up, but the marketplace is unreachable");
        return rc;
    }
    set_status(BOOTSTRAP_CONNECTED, "Connected to marketplace; registering hardware");

    /* Step 5a: Register this machine and request a machine-specific sync plan */
    {
        char desired_apps[256];
        char sync_report[192];
        load_desired_apps(desired_apps, sizeof(desired_apps));
        if (marketplace_sync_system(&manifest, "0.7.6", desired_apps,
                                    sync_report, sizeof(sync_report)) == HAL_OK) {
            persist_sync_report(sync_report);
            set_status(BOOTSTRAP_CONNECTED, sync_report);
        } else {
            persist_sync_report("Connected, but hardware registration did not finish.\n");
        }
    }

    if (need_drivers == 0) {
        hal_console_puts("  Machine registered. No extra drivers were needed.\n");
        marketplace_disconnect();
        set_status(BOOTSTRAP_COMPLETE, "Registered and ready: no extra drivers needed");
        return HAL_OK;
    }

    /* Step 5b: Send manifest, get driver recommendations */
    hal_console_puts("  Sending your hardware info to find matching drivers...\n");
    rc = marketplace_send_manifest(&manifest);
    if (rc != HAL_OK) {
        hal_console_puts("  Could not send hardware info. Continuing with built-in drivers.\n");
        marketplace_disconnect();
        set_status(BOOTSTRAP_FAILED, "Registered, but driver recommendations could not be fetched");
        return rc;
    }
    set_status(BOOTSTRAP_MANIFEST_SENT, "Hardware registered; downloading missing drivers");

    /* Step 6: Download and install each recommended driver */
    set_status(BOOTSTRAP_DOWNLOADING, "Downloading machine-specific drivers");
    uint32_t downloaded = 0;

    for (uint32_t i = 0; i < manifest.entry_count; i++) {
        manifest_entry_t *e = &manifest.entries[i];

        if (e->has_driver)
            continue; /* Already have a driver */

        hal_console_printf("  Downloading driver for device %04x:%04x...\n",
                           e->vendor_id, e->device_id);

        void *drv_data = NULL;
        uint64_t drv_size = 0;
        rc = marketplace_get_driver(e->vendor_id, e->device_id, &drv_data, &drv_size);
        if (rc != HAL_OK) {
            hal_console_printf("  No driver found for device %04x:%04x (skipping)\n",
                               e->vendor_id, e->device_id);
            continue;
        }

        /* Find the matching hal_device_t */
        hal_device_t *target_dev = NULL;
        for (uint32_t j = 0; j < count; j++) {
            if (devices[j].vendor_id == e->vendor_id &&
                devices[j].device_id == e->device_id) {
                target_dev = &devices[j];
                break;
            }
        }

        /* Load the driver */
        rc = driver_load_runtime(drv_data, drv_size, target_dev);
        if (rc == HAL_OK) {
            downloaded++;
            e->has_driver = 1;
            hal_console_printf("  Installed driver for device %04x:%04x\n",
                               e->vendor_id, e->device_id);
        } else {
            hal_console_printf("  Could not install driver for device %04x:%04x\n",
                               e->vendor_id, e->device_id);
        }

        /* Free the downloaded data */
        if (drv_data)
            hal_dma_free(drv_data, drv_size);
    }

    /* Step 7: Check for OS updates before disconnecting */
    {
        char update_url[256];
        update_url[0] = '\0';
        rc = marketplace_check_updates("0.1.0", update_url, sizeof(update_url));
        if (rc == HAL_OK && update_url[0]) {
            hal_console_printf("  OS update available: %s\n", update_url);
            /* OTA update download/apply deferred to next reboot cycle */
        }
    }

    marketplace_disconnect();

    hal_console_printf("  Downloaded and installed %u additional drivers.\n", downloaded);
    persist_sync_report("Registered and synchronized successfully.\n");
    set_status(BOOTSTRAP_COMPLETE, "Registered and synchronized successfully");
    return HAL_OK;
}

bootstrap_state_t ai_bootstrap_state(void)
{
    return g_state;
}

const char *ai_bootstrap_status_message(void)
{
    return g_status_message;
}
