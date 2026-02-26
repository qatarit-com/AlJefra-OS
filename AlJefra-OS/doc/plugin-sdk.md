# AlJefra OS Plugin SDK

This document describes how to develop, build, sign, and publish plugins
(drivers, GUI extensions, and system services) for AlJefra OS using the
`.ajdrv` package format.

---

## Table of Contents

1. [Overview](#overview)
2. [Package Format](#package-format)
3. [Plugin Types](#plugin-types)
4. [Kernel API Reference](#kernel-api-reference)
5. [Build Requirements](#build-requirements)
6. [Build Process](#build-process)
7. [Example: Hello World Driver](#example-hello-world-driver)
8. [Example: Storage Driver Skeleton](#example-storage-driver-skeleton)
9. [Critical Rules](#critical-rules)
10. [Signing](#signing)
11. [Testing](#testing)
12. [Publishing](#publishing)
13. [Troubleshooting](#troubleshooting)

---

## Overview

AlJefra OS can load drivers and plugins at runtime without rebooting. Plugins
are distributed as `.ajdrv` packages -- self-contained, signed, relocatable
binaries that the kernel loads into memory and links via a function-pointer
vtable.

The workflow:

1. Write a C source file that implements the `driver_ops_t` interface
2. Compile it as position-independent code (PIC)
3. Package it into a `.ajdrv` file with metadata and an Ed25519 signature
4. Upload it to the AlJefra Marketplace
5. Users download it through the OS, the kernel verifies the signature,
   loads the binary, and calls `init()`

---

## Package Format

A `.ajdrv` file has four sections laid out contiguously:

```
+------------------+  offset 0
| Header (64 B)    |  Magic, version, arch, category, sizes
+------------------+  offset 64
| Metadata (JSON)  |  Name, description, vendor/device IDs
+------------------+  offset 64 + meta_size
| PIC Binary       |  Position-independent ELF or flat binary
+------------------+  offset 64 + meta_size + bin_size
| Ed25519 Sig (64B)|  Signature over bytes [0 .. sig_offset)
+------------------+
```

### Header Fields (64 bytes)

| Offset | Size | Field             | Description                       |
|--------|------|-------------------|-----------------------------------|
| 0      | 4    | `magic`           | `0x4A444156` ("AJDV")             |
| 4      | 2    | `version`         | Package format version (1)        |
| 6      | 1    | `arch`            | 0=x86_64, 1=aarch64, 2=riscv64   |
| 7      | 1    | `category`        | Driver category (see enum below)  |
| 8      | 4    | `meta_size`       | Size of JSON metadata in bytes    |
| 12     | 4    | `bin_size`        | Size of PIC binary in bytes       |
| 16     | 2    | `vendor_id`       | PCI vendor ID (or 0x0000)         |
| 18     | 2    | `device_id`       | PCI device ID (or 0x0000)         |
| 20     | 44   | `reserved`        | Reserved, zero-filled             |

### Category Codes

| Code | Category      |
|------|---------------|
| 0    | Storage       |
| 1    | Network       |
| 2    | Input         |
| 3    | Display       |
| 4    | GPU           |
| 5    | Bus           |
| 6    | Other         |

### Metadata JSON

```json
{
    "name": "qemu_vga",
    "description": "QEMU Standard VGA display driver",
    "version": "1.0.0",
    "author": "AlJefra Team",
    "vendor_id": "0x1234",
    "device_id": "0x1111",
    "license": "MIT"
}
```

---

## Plugin Types

### Driver

The most common plugin type. Implements `driver_ops_t` to provide storage,
network, input, or display functionality for a specific hardware device.

### GUI Extension

Provides additional GUI widgets, themes, or desktop panels. Uses the kernel
API for framebuffer access and input polling.

### System Service

Background services such as a logging daemon, a health monitor, or an OTA
update agent. Uses the kernel API for timers, console, and network access.

---

## Kernel API Reference

When a runtime driver is loaded, the kernel passes it a pointer to a
`kernel_api_t` vtable. This is the driver's only interface to the kernel --
it must not call any kernel symbols directly.

```c
typedef struct {
    /* Console output */
    void (*puts)(const char *s);

    /* MMIO register access */
    uint32_t (*mmio_read32)(volatile void *addr);
    void     (*mmio_write32)(volatile void *addr, uint32_t val);
    uint8_t  (*mmio_read8)(volatile void *addr);
    void     (*mmio_write8)(volatile void *addr, uint8_t val);
    void     (*mmio_barrier)(void);      /* Memory barrier */

    /* DMA buffer allocation */
    void *(*dma_alloc)(uint64_t size, uint64_t *phys);
    void  (*dma_free)(void *ptr, uint64_t size);

    /* Timing */
    void     (*delay_us)(uint64_t us);   /* Busy-wait delay */
    uint64_t (*timer_ms)(void);          /* Monotonic ms clock */

    /* PCI / Bus */
    void          (*pci_enable)(hal_device_t *dev);
    volatile void *(*map_bar)(hal_device_t *dev, uint32_t idx);
    uint32_t      (*pci_read32)(uint32_t bdf, uint32_t reg);
    void          (*pci_write32)(uint32_t bdf, uint32_t reg, uint32_t val);
} kernel_api_t;
```

### Function Details

| Function        | Description                                          |
|-----------------|------------------------------------------------------|
| `puts`          | Print a null-terminated string to the kernel console |
| `mmio_read32`   | Read a 32-bit value from a memory-mapped register    |
| `mmio_write32`  | Write a 32-bit value to a memory-mapped register     |
| `mmio_read8`    | Read an 8-bit value from a memory-mapped register    |
| `mmio_write8`   | Write an 8-bit value to a memory-mapped register     |
| `mmio_barrier`  | Issue a memory barrier (fence) after MMIO writes     |
| `dma_alloc`     | Allocate a DMA-capable buffer; returns virt and phys |
| `dma_free`      | Free a previously allocated DMA buffer               |
| `delay_us`      | Busy-wait for the specified number of microseconds   |
| `timer_ms`      | Return current monotonic time in milliseconds        |
| `pci_enable`    | Enable bus mastering and memory space for a PCI dev  |
| `map_bar`       | Map a PCI BAR into the kernel virtual address space  |
| `pci_read32`    | Read a 32-bit PCI configuration register             |
| `pci_write32`   | Write a 32-bit PCI configuration register            |

---

## Build Requirements

- **GCC** (native or cross-compiler for the target architecture)
- **Mandatory flags**: `-fPIC -fno-tree-vectorize`
  - `-fPIC`: Generates position-independent code (required for relocation)
  - `-fno-tree-vectorize`: Prevents GCC from using absolute addresses for
    SIMD operations
- **Recommended flags**: `-O2 -Wall -ffreestanding -nostdlib -nostartfiles`
- **No standard library**: The plugin runs in kernel space with no libc

### Cross-Compilation

| Architecture | Compiler                     | Extra Flags                        |
|-------------|------------------------------|------------------------------------|
| x86_64      | `gcc`                        | `-m64 -march=x86-64 -mno-red-zone`|
| aarch64     | `aarch64-linux-gnu-gcc`      | `-march=armv8-a`                   |
| riscv64     | `riscv64-linux-gnu-gcc`      | `-march=rv64gc -mabi=lp64d`        |

---

## Build Process

Use the provided build script:

```bash
cd drivers/runtime/

./build_ajdrv.sh <source.c> <arch> <vendor_id> <device_id> <category> <name> <desc>
```

**Arguments:**

| Argument    | Description                                    |
|-------------|------------------------------------------------|
| `source.c`  | Path to the driver C source file               |
| `arch`      | Target: `x86_64`, `aarch64`, or `riscv64`     |
| `vendor_id` | PCI vendor ID in decimal (e.g., `4660` = 0x1234)|
| `device_id` | PCI device ID in decimal (e.g., `4369` = 0x1111)|
| `category`  | Category code (0-6, see table above)           |
| `name`      | Short name (e.g., `qemu_vga`)                  |
| `desc`      | Description string (quote if it has spaces)    |

**Example:**

```bash
./build_ajdrv.sh qemu_vga.c x86_64 1234 1111 3 qemu_vga "QEMU Standard VGA"
```

This produces `qemu_vga.ajdrv` in the current directory.

---

## Example: Hello World Driver

A minimal driver that prints a message on load:

```c
/* hello.c — Minimal AlJefra OS runtime driver */

#include <stdint.h>

/* Forward declaration of kernel API (provided at load time) */
typedef struct {
    void (*puts)(const char *s);
    uint32_t (*mmio_read32)(volatile void *addr);
    void     (*mmio_write32)(volatile void *addr, uint32_t val);
    uint8_t  (*mmio_read8)(volatile void *addr);
    void     (*mmio_write8)(volatile void *addr, uint8_t val);
    void     (*mmio_barrier)(void);
    void *(*dma_alloc)(uint64_t size, uint64_t *phys);
    void  (*dma_free)(void *ptr, uint64_t size);
    void     (*delay_us)(uint64_t us);
    uint64_t (*timer_ms)(void);
    void          (*pci_enable)(void *dev);
    volatile void *(*map_bar)(void *dev, uint32_t idx);
    uint32_t      (*pci_read32)(uint32_t bdf, uint32_t reg);
    void          (*pci_write32)(uint32_t bdf, uint32_t reg, uint32_t val);
} kernel_api_t;

/* Driver ops — must be runtime-initialized (see Critical Rules) */
typedef struct {
    const char *name;
    int         category;
    int       (*init)(void *dev);
    void      (*shutdown)(void);
} driver_ops_t;

static const kernel_api_t *g_api;
static driver_ops_t g_ops;

static int hello_init(void *dev) {
    (void)dev;
    g_api->puts("hello: driver loaded successfully!\n");
    return 0;
}

static void hello_shutdown(void) {
    g_api->puts("hello: shutting down\n");
}

/* Entry point — called by the kernel driver loader */
driver_ops_t *ajdrv_entry(const kernel_api_t *api, void *dev) {
    g_api = api;

    /* MUST runtime-initialize — no static initializers for pointers */
    g_ops.name     = "hello";
    g_ops.category = 6;  /* OTHER */
    g_ops.init     = hello_init;
    g_ops.shutdown = hello_shutdown;

    if (hello_init(dev) != 0)
        return (void *)0;

    return &g_ops;
}
```

Build it:

```bash
./build_ajdrv.sh hello.c x86_64 0 0 6 hello "Hello World test driver"
```

---

## Example: Storage Driver Skeleton

```c
/* mystore.c — Storage driver skeleton for AlJefra OS */

#include <stdint.h>

typedef struct { /* ... kernel_api_t fields ... */ } kernel_api_t;
typedef struct { /* ... driver_ops_t fields ... */ } driver_ops_t;

static const kernel_api_t *g_api;
static driver_ops_t g_ops;
static volatile void *g_bar;

static int mystore_init(void *dev) {
    g_api->pci_enable(dev);
    g_bar = g_api->map_bar(dev, 0);
    if (!g_bar) {
        g_api->puts("mystore: failed to map BAR0\n");
        return -1;
    }

    uint32_t cap = g_api->mmio_read32((volatile uint8_t *)g_bar + 0x00);
    g_api->puts("mystore: controller online\n");
    return 0;
}

static int64_t mystore_read(void *buf, uint64_t lba, uint32_t count) {
    /* Build command, submit to hardware, wait for completion */
    /* Use g_api->dma_alloc() for DMA buffers */
    /* Use g_api->mmio_write32() to poke doorbell registers */
    return count;  /* Return sectors read */
}

static int64_t mystore_write(const void *buf, uint64_t lba, uint32_t cnt) {
    /* Similar to read, but with write command */
    return cnt;
}

static void mystore_shutdown(void) {
    g_api->puts("mystore: shutdown\n");
}

driver_ops_t *ajdrv_entry(const kernel_api_t *api, void *dev) {
    g_api = api;

    g_ops.name     = "mystore";
    g_ops.category = 0;  /* STORAGE */
    g_ops.init     = mystore_init;
    g_ops.shutdown = mystore_shutdown;
    g_ops.read     = mystore_read;
    g_ops.write    = mystore_write;

    if (mystore_init(dev) != 0)
        return (void *)0;

    return &g_ops;
}
```

---

## Critical Rules

These rules are mandatory. Violating them will produce drivers that crash
at load time or behave incorrectly.

### 1. No Static Initializers for Pointer Fields

**Wrong** (generates absolute addresses baked at link time):

```c
static driver_ops_t g_ops = {
    .name = "mydriver",
    .init = mydriver_init,     /* ABSOLUTE ADDRESS -- will crash */
};
```

**Correct** (generates RIP-relative LEA instructions):

```c
static driver_ops_t g_ops;

driver_ops_t *ajdrv_entry(const kernel_api_t *api, void *dev) {
    g_ops.name = "mydriver";
    g_ops.init = mydriver_init;   /* Runtime assignment -- safe */
    /* ... */
}
```

### 2. Always Use -fPIC -fno-tree-vectorize

Without `-fPIC`, the compiler generates absolute addresses that cannot be
relocated. Without `-fno-tree-vectorize`, auto-vectorization may introduce
absolute address references even in PIC mode.

### 3. No Standard Library Calls

There is no `libc` in kernel space. Do not call `printf`, `malloc`, `memcpy`,
or any standard library function. Use only the `kernel_api_t` vtable.

If you need `memcpy` or `memset`, implement them locally in your source file.

### 4. Entry Point Must Be `ajdrv_entry`

The kernel looks for the symbol `ajdrv_entry` when loading a `.ajdrv` package.
It must have this exact signature:

```c
driver_ops_t *ajdrv_entry(const kernel_api_t *api, void *dev);
```

Return a pointer to your `driver_ops_t` on success, or `NULL` (`(void *)0`)
on failure.

---

## Signing

All `.ajdrv` packages must be signed with Ed25519 before they can be loaded
by the kernel. Unsigned packages are rejected.

### Signing Process

1. Build the `.ajdrv` package (header + metadata + binary, no signature yet)
2. Sign the package with the AlJefra Store signing key:
   ```bash
   # Using the provided signing tool
   python3 server/sign_ajdrv.py --key store_signing_key.pem --input mydriver.ajdrv
   ```
3. The tool appends a 64-byte Ed25519 signature to the package

### Verification

The kernel verifies the signature at load time using the Store public key
embedded in the kernel image. The verification implementation is in
`store/verify.c` (full Ed25519 over GF(2^255-19) with SHA-512).

### For Development / Testing

During development, you can disable signature verification in QEMU by
building the kernel with the `NO_VERIFY` flag. This is for testing only and
must never be used in production.

---

## Testing

### Local Testing in QEMU

1. Build the kernel with marketplace support:
   ```bash
   make ARCH=x86_64
   ```

2. Start the marketplace server locally:
   ```bash
   cd server && python3 app.py
   ```

3. Upload your `.ajdrv` to the local marketplace:
   ```bash
   curl -X POST -F "file=@mydriver.ajdrv" http://localhost:5000/v1/drivers/upload
   ```

4. Boot the kernel in QEMU -- it will connect to the local marketplace,
   discover the driver, download it, verify the signature, and load it.

### Verifying Driver Output

Use `-serial stdio` with QEMU to see kernel console output. Your driver's
`puts()` calls will appear in the serial output.

---

## Publishing

To publish a driver to the AlJefra Marketplace:

1. **Test thoroughly** in QEMU on the target architecture
2. **Sign the package** with your developer key
3. **Submit for review** via the marketplace API:
   ```bash
   curl -X POST -F "file=@mydriver.ajdrv" \
     -F "author=Your Name" \
     -F "source_url=https://github.com/you/mydriver" \
     https://store.aljefra.com/v1/drivers/upload
   ```
4. **Community review**: Other developers can review and rate your driver
5. **Approval**: Once approved, the driver appears in the public marketplace

### Marketplace API Endpoints

| Method | Endpoint                    | Description                 |
|--------|-----------------------------|-----------------------------|
| GET    | `/v1/drivers`               | List all available drivers  |
| GET    | `/v1/drivers/<id>`          | Get driver details          |
| POST   | `/v1/drivers/upload`        | Upload a new driver         |
| GET    | `/v1/drivers/search`        | Search by name/vendor/arch  |
| POST   | `/v1/drivers/<id>/review`   | Submit a community review   |
| GET    | `/v1/drivers/<id>/reviews`  | Get reviews for a driver    |
| POST   | `/v1/evolve`                | Submit an evolution proposal |
| GET    | `/v1/updates`               | Check for OTA updates       |
| GET    | `/v1/metrics`               | System metrics              |

---

## Troubleshooting

### Driver crashes immediately on load

- Check for static initializers with function pointers (see Critical Rules #1)
- Verify you compiled with `-fPIC -fno-tree-vectorize`
- Ensure the entry point is named `ajdrv_entry`

### "Signature verification failed"

- Make sure the package is signed with a recognized key
- Check that the file was not modified after signing
- For testing, build with `NO_VERIFY` flag

### Driver loads but hardware does not respond

- Verify `pci_enable()` was called before accessing BARs
- Check that `map_bar()` returned a non-NULL pointer
- Use `mmio_barrier()` after writing configuration registers
- Confirm vendor/device IDs match the actual hardware

### Build fails with "relocation R_X86_64_32 against .rodata"

- You are missing `-fPIC`. Add it to your CFLAGS.

### Build fails with undefined reference to `memcpy`

- Implement `memcpy` locally in your source file. There is no libc.

---

*AlJefra OS Plugin SDK -- Built in Qatar. Built for the world.*
*Qatar IT -- www.QatarIT.com*
