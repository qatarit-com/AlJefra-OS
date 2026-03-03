# AlJefra OS Developer Guide

## Overview

AlJefra OS is a lightweight, multi-architecture operating system kernel supporting
x86-64, ARM64, and RISC-V 64. This guide covers everything needed to set up a
development environment, build the kernel, run it under emulation, and contribute code.

---

## Prerequisites

### Required Packages

Install the following tools on a Debian/Ubuntu-based system:

```bash
# Native build tools
sudo apt update
sudo apt install -y \
    build-essential \
    nasm \
    gcc \
    make \
    python3 \
    python3-pip \
    python3-venv \
    git

# Cross-compilation toolchains
sudo apt install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu

# QEMU emulators
sudo apt install -y \
    qemu-system-x86 \
    qemu-system-arm \
    qemu-system-misc    # Includes qemu-system-riscv64

# Python packages (for marketplace server)
pip3 install flask
```

### Fedora/RHEL

```bash
sudo dnf install -y \
    @development-tools nasm gcc make python3 python3-pip git \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
    qemu-system-x86 qemu-system-aarch64 qemu-system-riscv
```

### macOS (via Homebrew)

```bash
brew install nasm qemu python3
# Cross toolchains via crosstool-ng or Docker
```

### Minimum Versions

| Tool                       | Minimum Version | Check Command                        |
|----------------------------|----------------|--------------------------------------|
| GCC (native)               | 12+            | `gcc --version`                      |
| NASM                       | 2.15+          | `nasm --version`                     |
| Make                       | 4.3+           | `make --version`                     |
| aarch64-linux-gnu-gcc      | 12+            | `aarch64-linux-gnu-gcc --version`    |
| riscv64-linux-gnu-gcc      | 12+            | `riscv64-linux-gnu-gcc --version`    |
| QEMU                       | 7.0+           | `qemu-system-x86_64 --version`      |
| Python                     | 3.8            | `python3 --version`                  |
| Flask                      | 2.0            | `python3 -c "import flask; print(flask.__version__)"` |

---

## Getting the Source

```bash
git clone https://github.com/qatarit-com/AlJefra-OS.git
cd AlJefra-OS
```

---

## Project Structure

```
AlJefra-OS/
+-- arch/                       # Architecture-specific code
|   +-- x86_64/                 # x86-64 HAL implementation
|   |   +-- boot.S              # Multiboot2 boot stub
|   |   +-- hal_init.c          # HAL initialization
|   |   +-- cpu.c               # CPU setup, GDT, IDT
|   |   +-- interrupt.c         # APIC interrupt controller
|   |   +-- timer.c             # APIC timer / HPET
|   |   +-- bus.c               # PCI enumeration (I/O port method)
|   |   +-- io.c                # Port I/O (inb/outb)
|   |   +-- mmu.c               # 4-level page tables (PML4)
|   |   +-- smp.c               # AP startup via SIPI
|   |   +-- console.c           # VGA text + serial
|   |   +-- linker.ld           # Linker script
|   +-- aarch64/                # ARM64 HAL implementation
|   |   +-- boot.S              # DTB-aware boot stub
|   |   +-- hal_init.c          # HAL initialization
|   |   +-- cpu.c               # Exception level setup
|   |   +-- interrupt.c         # GICv2 driver
|   |   +-- timer.c             # ARM generic timer
|   |   +-- bus.c               # PCI ECAM enumeration
|   |   +-- io.c                # MMIO primitives
|   |   +-- mmu.c               # 4-level translation tables
|   |   +-- smp.c               # PSCI-based core startup
|   |   +-- console.c           # PL011 UART
|   |   +-- linker.ld           # Linker script
|   +-- riscv64/                # RISC-V 64 HAL implementation
|       +-- boot.S              # OpenSBI-aware boot stub
|       +-- hal_init.c          # HAL initialization
|       +-- cpu.c               # CSR setup
|       +-- interrupt.c         # PLIC driver
|       +-- timer.c             # CLINT timer
|       +-- bus.c               # PCI + VirtIO MMIO enumeration
|       +-- io.c                # MMIO primitives
|       +-- mmu.c               # Sv39/Sv48 page tables
|       +-- smp.c               # Hart startup via SBI
|       +-- console.c           # NS16550 UART
|       +-- linker.ld           # Linker script
+-- kernel/                     # Architecture-independent kernel
|   +-- main.c                  # kernel_main() entry point
|   +-- klog.c                  # Kernel ring buffer logging
|   +-- panic.c                 # Panic handler with backtrace
|   +-- memprotect.c            # Memory protection (NX, WP, guard pages)
|   +-- ota.c                   # OTA update handler
|   +-- shell.c                 # Interactive kernel shell
+-- drivers/                    # Device drivers
|   +-- storage/                # Block device drivers
|   |   +-- virtio_blk.c        # VirtIO block storage
|   +-- network/                # Network drivers
|   |   +-- virtio_net.c        # VirtIO network
|   |   +-- e1000.c             # Intel E1000
|   |   +-- aes_ccmp.c          # WPA2 AES-CCMP encryption
|   +-- display/                # Display drivers
|   |   +-- qemu_vga.c          # QEMU standard VGA (1234:1111)
|   +-- input/                  # Input device drivers
+-- include/                    # Header files
|   +-- hal.h                   # HAL interface declarations
|   +-- kernel.h                # Kernel types and macros
|   +-- pci.h                   # PCI structures and constants
|   +-- ed25519_key.h           # Embedded root verification key
|   +-- driver.h                # Driver registration interface
+-- store/                      # Ed25519 verification library
|   +-- verify.c                # Ed25519 implementation (RFC 8032)
+-- server/                     # Marketplace server
|   +-- app.py                  # Flask REST API
|   +-- requirements.txt        # Python dependencies
|   +-- docker-compose.yml      # Docker deployment
|   +-- Dockerfile              # Container build
+-- tools/                      # Build and packaging tools
|   +-- ajdrv_builder.py        # .ajdrv package builder
+-- net/                        # Network stack
|   +-- tls.c                   # TLS 1.2 (BearSSL integration)
+-- doc/                        # Documentation
|   +-- developer_guide.md      # This file
|   +-- porting_guide.md        # How to port to a new architecture
|   +-- memory_maps.md          # Memory layouts for all architectures
|   +-- security_model.md       # Security architecture
|   +-- marketplace_spec.md     # Marketplace API specification
|   +-- hal_spec.md             # HAL interface specification
|   +-- driver_guide.md         # How to write drivers
|   +-- architecture.md         # System architecture overview
+-- build/                      # Build output (generated)
|   +-- x86_64/
|   |   +-- kernel.elf          # ~147 KB
|   +-- aarch64/
|   |   +-- kernel.elf          # ~153 KB
|   +-- riscv64/
|       +-- kernel.elf          # ~129 KB
+-- Makefile                    # Root build system
```

---

## Building

### Build for a Single Architecture

```bash
# x86-64 (default)
make

# ARM64
make ARCH=aarch64

# RISC-V 64
make ARCH=riscv64
```

### Build for All Architectures

```bash
make all-arch
```

This produces binaries for all three architectures:

```
build/x86_64/kernel.elf     # ~147 KB
build/aarch64/kernel.elf    # ~153 KB
build/riscv64/kernel.elf    # ~129 KB
```

### Clean

```bash
# Clean current architecture build
make clean

# Clean all architectures
make clean-all
```

### Build Options

| Variable    | Default    | Description                              |
|-------------|-----------|------------------------------------------|
| `ARCH`      | `x86_64`  | Target architecture                      |
| `DEBUG`     | `0`       | Set to `1` for debug symbols (`-g`)     |
| `OPTIMIZE`  | `-O2`     | Optimization level                       |
| `VERBOSE`   | `0`       | Set to `1` to print full compiler commands |

Example with options:

```bash
make ARCH=aarch64 DEBUG=1 VERBOSE=1
```

---

## Running on QEMU

### x86-64

```bash
qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 256M \
    -kernel build/x86_64/kernel.elf \
    -serial stdio \
    -no-reboot \
    -d int,cpu_reset \
    -D qemu_log.txt
```

Or using the Makefile target:

```bash
make run
```

### ARM64

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a72 \
    -m 256M \
    -kernel build/aarch64/kernel.elf \
    -nographic
```

Or:

```bash
make ARCH=aarch64 run
```

### RISC-V 64

```bash
qemu-system-riscv64 \
    -machine virt \
    -cpu rv64 \
    -m 256M \
    -bios default \
    -kernel build/riscv64/kernel.elf \
    -nographic
```

Or:

```bash
make ARCH=riscv64 run
```

### QEMU Networking

To test network drivers and marketplace connectivity:

```bash
qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 256M \
    -kernel build/x86_64/kernel.elf \
    -serial stdio \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -device e1000,netdev=net0
```

### Exiting QEMU

Press `Ctrl+A` then `X` to terminate QEMU when using `-nographic` or `-serial stdio`.

---

## Running the Marketplace Server

### Local Development

```bash
cd server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python3 app.py
```

The server starts at `http://localhost:8081`. You should see:

```
 * Running on http://0.0.0.0:8081
 * AlJefra Marketplace API v1 ready
```

### Docker

```bash
cd server
docker-compose -f docker-compose.yml up
```

To run in the background:

```bash
docker-compose -f docker-compose.yml up -d
```

To stop:

```bash
docker-compose -f docker-compose.yml down
```

### Testing the API

```bash
# List all drivers
curl http://localhost:8081/v1/catalog | python3 -m json.tool

# Submit a hardware manifest
curl -X POST http://localhost:8081/v1/manifest \
    -H "Content-Type: application/json" \
    -d '{
        "arch": "x86_64",
        "devices": [
            {"type": "pci", "vendor_id": "0x8086", "device_id": "0x10D3"},
            {"type": "pci", "vendor_id": "0x1AF4", "device_id": "0x1001"}
        ],
        "os_version": "1.0"
    }' | python3 -m json.tool

# Download a specific driver
curl -O http://localhost:8081/v1/drivers/8086/10D3/x86_64
```

---

## Code Style

AlJefra OS follows a consistent C coding style across the entire codebase.

### Formatting Rules

| Rule                  | Standard                                    |
|-----------------------|---------------------------------------------|
| Brace style           | K&R (opening brace on same line)            |
| Indentation           | 4 spaces (no tabs)                          |
| Line length           | Maximum 80 characters                       |
| License header        | SPDX identifier in every file               |
| Naming (functions)    | `snake_case`                                |
| Naming (macros)       | `UPPER_SNAKE_CASE`                          |
| Naming (types)        | `snake_case_t` for typedefs                 |
| Naming (constants)    | `UPPER_SNAKE_CASE`                          |
| Comments              | `/* C-style block comments */`              |
| Pointer declarations  | `int *ptr` (asterisk with variable)         |

### Example

```c
// SPDX-License-Identifier: MIT
// Copyright (c) AlJefra Foundation

#include <hal.h>
#include <kernel.h>

#define MAX_DEVICES 256

/*
 * Enumerate PCI devices on the given bus segment.
 * Returns the number of devices found.
 */
static uint32_t pci_scan_bus(uint8_t bus) {
    uint32_t count = 0;

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t vendor = pci_config_read(bus, dev, 0, 0);
        if ((vendor & 0xFFFF) == 0xFFFF) {
            continue;
        }
        count++;
        klog(KLOG_INFO, "PCI: %02x:%02x.0 vendor=%04x device=%04x\n",
             bus, dev, vendor & 0xFFFF, (vendor >> 16) & 0xFFFF);
    }

    return count;
}
```

### SPDX License Headers

Every source file must begin with an SPDX license identifier:

```c
// SPDX-License-Identifier: MIT
```

---

## How to Add a Driver

### Step 1: Create the Source File

Create a new `.c` file in the appropriate `drivers/` subdirectory:

```
drivers/
    storage/    # Block devices
    network/    # NICs, WiFi
    display/    # GPU, framebuffer
    input/      # Keyboard, mouse
    bus/        # Host controllers
```

For example, a new USB host controller driver:

```bash
touch drivers/bus/xhci.c
```

### Step 2: Implement the Driver Interface

Every driver must implement the standard driver entry point:

```c
// SPDX-License-Identifier: MIT

#include <driver.h>
#include <hal.h>
#include <kernel.h>

#define XHCI_VENDOR_ID  0x1B36
#define XHCI_DEVICE_ID  0x000D

/*
 * Driver initialization function.
 * Called by the kernel after the driver is loaded and verified.
 */
int xhci_init(uint16_t vendor_id, uint16_t device_id, void *mmio_base) {
    klog(KLOG_INFO, "xHCI: Initializing USB 3.0 controller at %p\n",
         mmio_base);

    // Read capability registers
    uint32_t caplength = mmio_read32(mmio_base);

    // Initialize operational registers
    // ...

    klog(KLOG_INFO, "xHCI: Controller ready\n");
    return 0;  // Success
}

/*
 * Driver metadata for the kernel's driver registry.
 */
static const struct driver_info xhci_driver = {
    .name      = "xhci",
    .vendor_id = XHCI_VENDOR_ID,
    .device_id = XHCI_DEVICE_ID,
    .category  = DRIVER_CAT_BUS,
    .init      = xhci_init,
};

REGISTER_DRIVER(xhci_driver);
```

### Step 3: Update the Makefile

Add the new source file to the driver build list in the Makefile:

```makefile
DRIVER_SRCS += drivers/bus/xhci.c
```

### Step 4: Build and Test

```bash
make
make run
```

Verify in the QEMU output that the driver initializes successfully.

### Step 5: Package as .ajdrv (Optional)

To distribute the driver through the marketplace:

```bash
python3 tools/ajdrv_builder.py \
    --name "xhci" \
    --arch x86_64 \
    --category bus \
    --vendor-id 0x1B36 \
    --device-id 0x000D \
    --source drivers/bus/xhci.c \
    --key keys/publisher_private.pem \
    --output xhci.ajdrv
```

---

## How to Add a HAL Function

If you need a new hardware abstraction that does not exist in the current HAL:

### Step 1: Declare in include/hal.h

```c
// New HAL function: reset the system
void hal_system_reset(void);
```

### Step 2: Implement in Each Architecture

Add the implementation to each `arch/*/` directory:

**arch/x86_64/cpu.c:**
```c
void hal_system_reset(void) {
    // Triple fault to reset x86
    io_outb(0x64, 0xFE);  // Keyboard controller reset
}
```

**arch/aarch64/cpu.c:**
```c
void hal_system_reset(void) {
    // PSCI system reset
    asm volatile("mov x0, #0x84000009; hvc #0");
}
```

**arch/riscv64/cpu.c:**
```c
void hal_system_reset(void) {
    // SBI shutdown
    asm volatile("li a7, 0x08; ecall");
}
```

### Step 3: Build All Architectures

```bash
make all-arch
```

Ensure the kernel links and boots on all three architectures.

---

## How to Modify the Kernel

### Adding a Kernel Module

1. Create a new `.c` file in `kernel/`:
   ```bash
   touch kernel/scheduler.c
   ```

2. Add a corresponding header in `include/`:
   ```bash
   touch include/scheduler.h
   ```

3. Add the source to the Makefile:
   ```makefile
   KERNEL_SRCS += kernel/scheduler.c
   ```

4. Call the module's init function from `kernel/main.c`:
   ```c
   #include <scheduler.h>

   void kernel_main(void) {
       hal_init();
       // ...
       scheduler_init();
   }
   ```

---

## Git Workflow

### Fork and Clone

1. Fork the repository on GitHub.
2. Clone your fork:
   ```bash
   git clone https://github.com/<your-username>/AlJefra-OS.git
   cd AlJefra-OS
   ```

3. Add the upstream remote:
   ```bash
   git remote add upstream https://github.com/qatarit-com/AlJefra-OS.git
   ```

### Branch

Create a feature branch from `main`:

```bash
git checkout -b feature/my-new-driver
```

### Commit

Write clear, descriptive commit messages:

```
Add xHCI USB 3.0 host controller driver

Implement basic xHCI initialization for QEMU's xHCI controller
(1B36:000D). Supports capability register parsing and operational
register setup. Tested on QEMU with qemu-xhci device.
```

### Push and Create a Pull Request

```bash
git push origin feature/my-new-driver
```

Then open a Pull Request on GitHub against the `main` branch of the upstream repository.

### PR Checklist

Before submitting a PR, verify:

- [ ] Code follows the K&R C style with 4-space indentation.
- [ ] All lines are 80 characters or fewer.
- [ ] SPDX license header is present in every new file.
- [ ] The kernel builds for all three architectures (`make all-arch`).
- [ ] The kernel boots successfully on QEMU for all affected architectures.
- [ ] No compiler warnings with `-Wall -Wextra`.
- [ ] Commit messages are clear and descriptive.
- [ ] Documentation is updated if a public interface changed.

---

## Testing

### QEMU Boot Verification

The primary test method is booting the kernel on QEMU and verifying console output.

**Automated test for all architectures:**

```bash
#!/bin/bash
# test_boot.sh - Verify kernel boots on all architectures

set -e

TIMEOUT=10  # seconds

for arch in x86_64 aarch64 riscv64; do
    echo "Testing $arch..."
    make ARCH=$arch clean
    make ARCH=$arch

    case $arch in
        x86_64)
            timeout $TIMEOUT qemu-system-x86_64 \
                -machine q35 -m 256M -nographic \
                -kernel build/x86_64/kernel.elf \
                -no-reboot 2>&1 | tee /tmp/boot_$arch.log || true
            ;;
        aarch64)
            timeout $TIMEOUT qemu-system-aarch64 \
                -machine virt -cpu cortex-a72 -m 256M -nographic \
                -kernel build/aarch64/kernel.elf \
                2>&1 | tee /tmp/boot_$arch.log || true
            ;;
        riscv64)
            timeout $TIMEOUT qemu-system-riscv64 \
                -machine virt -m 256M -nographic -bios default \
                -kernel build/riscv64/kernel.elf \
                2>&1 | tee /tmp/boot_$arch.log || true
            ;;
    esac

    if grep -q "AlJefra OS" /tmp/boot_$arch.log; then
        echo "  PASS: $arch boot successful"
    else
        echo "  FAIL: $arch boot failed"
        exit 1
    fi
done

echo "All architectures boot successfully."
```

### Marketplace API Testing

```bash
cd server
python3 -m pytest tests/ -v
```

Or manually with curl (see the marketplace section above).

---

## Common Issues and Troubleshooting

### Build Errors

**Problem:** `nasm: command not found`
**Solution:** Install NASM: `sudo apt install nasm`

**Problem:** `aarch64-linux-gnu-gcc: command not found`
**Solution:** Install the cross-compiler: `sudo apt install gcc-aarch64-linux-gnu`

**Problem:** `riscv64-linux-gnu-gcc: command not found`
**Solution:** Install the cross-compiler: `sudo apt install gcc-riscv64-linux-gnu`

**Problem:** `undefined reference to 'hal_init'`
**Solution:** Ensure the `ARCH` variable is set correctly. Each architecture provides
its own `hal_init.c`.

**Problem:** Linker error about missing `_start` symbol
**Solution:** Check that `boot.S` defines a global `_start` label and the linker script
uses `ENTRY(_start)`.

### QEMU Errors

**Problem:** `qemu-system-x86_64: command not found`
**Solution:** Install QEMU: `sudo apt install qemu-system-x86`

**Problem:** QEMU starts but no output appears
**Solution:** Ensure you are using `-serial stdio` (x86) or `-nographic` (ARM/RISC-V).
Check that `console_init()` is the first HAL function called.

**Problem:** QEMU crashes with "guest hasn't initialized the display"
**Solution:** Use `-nographic` or `-display none -serial stdio`. AlJefra OS uses serial
console by default on ARM64 and RISC-V.

**Problem:** RISC-V kernel does not start
**Solution:** Ensure `-bios default` is passed so OpenSBI runs first. The kernel expects
to be entered in S-mode, not M-mode.

**Problem:** Triple fault on x86-64 boot
**Solution:** Check your GDT and IDT setup. Enable QEMU debug logging:
`-d int,cpu_reset -D qemu_log.txt` and examine the log.

### Marketplace Errors

**Problem:** `ModuleNotFoundError: No module named 'flask'`
**Solution:** Install Flask: `pip3 install flask` (or use the virtual environment).

**Problem:** Port 8081 already in use
**Solution:** Kill the existing process or set a different port:
`ALJEFRA_STORE_PORT=9090 python3 app.py`

**Problem:** Driver upload returns 403
**Solution:** The publisher key is not registered or the Ed25519 signature is invalid.
Verify with: `python3 tools/ajdrv_builder.py --verify --input driver.ajdrv --pubkey key.pem`

### Debugging Tips

1. **Serial output is your best friend.** Add `klog()` calls liberally during
   development.

2. **Use QEMU's built-in GDB server** for source-level debugging:
   ```bash
   # Terminal 1: Start QEMU with GDB server
   qemu-system-x86_64 -s -S -kernel build/x86_64/kernel.elf -nographic

   # Terminal 2: Connect GDB
   gdb build/x86_64/kernel.elf
   (gdb) target remote :1234
   (gdb) break kernel_main
   (gdb) continue
   ```

3. **Check the QEMU monitor** for hardware state:
   Press `Ctrl+A` then `C` to enter the QEMU monitor, then:
   ```
   info registers    # Dump CPU registers
   info mem          # Show memory mappings
   info pci          # List PCI devices
   ```

4. **Build with debug symbols** for better backtraces:
   ```bash
   make DEBUG=1
   ```

5. **Read the panic output.** When the kernel panics, it prints register values and a
   backtrace. Cross-reference addresses with `objdump -d build/<arch>/kernel.elf`.

---

## Contact and Resources

| Resource                     | Location                                          |
|------------------------------|---------------------------------------------------|
| Source code                  | https://github.com/qatarit-com/AlJefra-OS        |
| Issue tracker                | https://github.com/qatarit-com/AlJefra-OS/issues |
| Marketplace (production)     | https://store.aljefra.com                         |
| Documentation                | `doc/` directory in the repository                |
| HAL specification            | `doc/hal_spec.md`                                 |
| Driver development guide     | `doc/driver_guide.md`                             |
| Security model               | `doc/security_model.md`                           |
| Architecture memory maps     | `doc/memory_maps.md`                              |
| Porting to new architectures | `doc/porting_guide.md`                            |
| Marketplace API              | `doc/marketplace_spec.md`                         |
