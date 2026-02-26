# Contributing to AlJefra OS

AlJefra OS welcomes contributions from developers worldwide. Whether you are
fixing a typo, adding a driver, improving documentation, or proposing a new
feature, your work helps build the first AI-native, self-evolving operating
system.

This guide explains how to contribute effectively.

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Architecture Overview](#architecture-overview)
3. [Code Style](#code-style)
4. [Building and Testing](#building-and-testing)
5. [Pull Request Process](#pull-request-process)
6. [Code Review Guidelines](#code-review-guidelines)
7. [Commit Message Format](#commit-message-format)
8. [Areas Needing Help](#areas-needing-help)
9. [Communication](#communication)
10. [License](#license)

---

## Getting Started

### Prerequisites

Install the build toolchain on a Debian/Ubuntu host:

```bash
sudo apt install nasm gcc make qemu-system-x86 qemu-system-arm qemu-system-misc \
  aarch64-linux-gnu-gcc riscv64-linux-gnu-gcc python3 python3-pip
```

### Clone the Repository

```bash
git clone https://github.com/QatarIT/AlJefra-OS.git
cd AlJefra-OS
```

### First Build

```bash
# Build for x86-64 (default)
make ARCH=x86_64

# Or build all three architectures
make all-arch
```

### First Boot (QEMU)

```bash
# x86-64
qemu-system-x86_64 -machine q35 -cpu Westmere -m 256 -smp 1 \
  -serial stdio -display none -kernel build/x86_64/bin/kernel_x86_64.bin

# ARM64
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 256 \
  -serial stdio -display none -kernel build/aarch64/bin/kernel_aarch64.bin

# RISC-V 64
qemu-system-riscv64 -M virt -m 256 \
  -serial stdio -display none -kernel build/riscv64/bin/kernel_riscv64.bin
```

If you see the boot banner and a `Ready` prompt, your environment is working.

---

## Architecture Overview

AlJefra OS is organized into layers:

```
Applications / AI Agent
        |
   Kernel Core  (C, portable)
   scheduler - syscalls - driver loader
        |
   HAL (Hardware Abstraction Layer)
   9 headers: cpu, interrupt, timer, bus, io, mmu, smp, console, hal
        |
  +----------+----------+----------+
  | x86-64   | AArch64  | RISC-V   |
  | boot.S   | boot.S   | boot.S   |
  | APIC     | GIC      | PLIC     |
  | HPET     | GenTimer  | CLINT   |
  +----------+----------+----------+
```

### Key Directories

| Path                  | Description                                  |
|-----------------------|----------------------------------------------|
| `hal/`                | Hardware Abstraction Layer headers            |
| `arch/x86_64/`        | x86-64 architecture implementation            |
| `arch/aarch64/`       | ARM64 architecture implementation             |
| `arch/riscv64/`       | RISC-V 64-bit architecture implementation     |
| `kernel/`             | Portable kernel core (scheduler, loader)      |
| `drivers/`            | Portable C drivers (storage, net, input, etc) |
| `drivers/runtime/`    | Runtime .ajdrv build tools                    |
| `store/`              | Marketplace client + Ed25519 verification     |
| `net/`                | TCP/IP, DHCP, DNS                             |
| `ai/`                 | AI bootstrap agent                            |
| `server/`             | Marketplace Flask REST API                    |
| `src/aljefra/`        | x86-64 ASM kernel (legacy/standalone)         |
| `programs/netstack/`  | Network stack + Claude API agent              |
| `doc/`                | Architecture and API documentation            |
| `website/`            | Static site for os.aljefra.com                |

---

## Code Style

### C Code

- **Style**: K&R (opening brace on the same line as the statement)
- **Indentation**: 4 spaces (no tabs)
- **Line length**: 80 characters maximum
- **Naming**: `snake_case` for functions and variables, `UPPER_SNAKE` for macros
- **File guards**: `#ifndef ALJEFRA_<MODULE>_H` / `#define` / `#endif`
- **Comments**: `/* C89-style block comments */` for multi-line, `//` for single-line
- **Types**: Use `<stdint.h>` fixed-width types (`uint32_t`, `int64_t`, etc.)
- **Headers**: Include the narrowest header needed, not `hal.h` when `cpu.h` suffices

Example:

```c
/* drivers/storage/example.c — Example storage driver */

#include "../../hal/bus.h"
#include "../../hal/io.h"

static uint32_t example_read_reg(volatile void *base, uint32_t off) {
    return hal_mmio_read32((volatile uint8_t *)base + off);
}

hal_status_t example_init(hal_device_t *dev) {
    volatile void *bar = hal_map_bar(dev, 0);
    if (!bar)
        return HAL_ERR_NO_DEVICE;

    uint32_t version = example_read_reg(bar, 0x00);
    hal_console_puts("example: version ");
    /* ... */
    return HAL_OK;
}
```

### Assembly (NASM, Intel syntax)

- **Syntax**: NASM Intel syntax (used in the x86-64 ASM kernel)
- **Indentation**: Tab characters for instructions, labels at column 0
- **Registers**: Lowercase (`rax`, `rdi`, `rsi`)
- **Comments**: `;` at end of line or on a dedicated line
- **Sections**: Use `SECTION .text` / `SECTION .data` / `SECTION .bss`
- **Labels**: `align 16` before hot-path entry points

Example:

```nasm
; pci_read32 — Read a 32-bit PCI config register
; Input:  EDI = BDF (bus/device/function), ESI = register offset
; Output: EAX = value
align 16
pci_read32:
	mov eax, edi
	shl eax, 8
	or eax, esi
	or eax, 0x80000000
	mov dx, 0x0CF8
	out dx, eax
	mov dx, 0x0CFC
	in eax, dx
	ret
```

### GCC Assembly (.S files)

- **Syntax**: GNU AS (AT&T syntax for ARM64 and RISC-V `.S` files)
- **Indentation**: Tab characters
- **Assembled with GCC**, not NASM

---

## Building and Testing

### Build Commands

```bash
make ARCH=x86_64       # Build x86-64 kernel + drivers
make ARCH=aarch64      # Build ARM64 kernel + drivers
make ARCH=riscv64      # Build RISC-V kernel + drivers
make all-arch          # Build all three
make clean             # Remove build artifacts
```

### Testing Requirements

Every pull request must:

1. **Compile cleanly** on all three architectures with no warnings (`-Wall -Werror`)
2. **Boot successfully** in QEMU for any architecture the change touches
3. **Not regress** existing functionality (boot, device scan, driver load)
4. **Include QEMU test commands** in the PR description if adding new hardware support

### Running Tests

```bash
# Boot test (x86-64 example — should print banner and reach Ready)
timeout 10 qemu-system-x86_64 -machine q35 -cpu Westmere -m 256 \
  -smp 1 -serial stdio -display none -kernel build/x86_64/bin/kernel_x86_64.bin
```

Verify the output includes the boot banner and `Ready` prompt.

---

## Pull Request Process

1. **Fork** the repository on GitHub
2. **Create a feature branch** from `main`:
   ```bash
   git checkout -b feature/my-driver
   ```
3. **Make your changes** following the code style above
4. **Test locally** in QEMU on the relevant architecture(s)
5. **Commit** with a descriptive message (see format below)
6. **Push** to your fork and open a pull request against `main`
7. **Fill in the PR template**: describe what changed, why, and how to test it
8. **Respond to review feedback** promptly

### PR Checklist

- [ ] Code follows the project style guide
- [ ] All modified architectures boot cleanly in QEMU
- [ ] No new compiler warnings
- [ ] Commit messages follow the required format
- [ ] Documentation updated if adding a new API or driver

---

## Code Review Guidelines

- All pull requests require **at least one approving review** before merge
- Reviewers should check:
  - Correctness: Does the code do what the PR claims?
  - Style: Does it follow C/ASM conventions above?
  - Safety: No undefined behavior, no unbounded buffers, no missing NULL checks
  - Architecture: Changes to HAL must work across all three architectures
  - Testing: Has the author confirmed QEMU boot?
- Be respectful and constructive in reviews
- Use "Request Changes" only for issues that must be fixed before merge
- Use "Comment" for suggestions and non-blocking feedback

---

## Commit Message Format

Follow this format:

```
component: short description

Optional longer explanation (wrap at 72 characters).
Explain the "why", not just the "what".
```

**Component prefixes:**

| Prefix          | Scope                                |
|-----------------|--------------------------------------|
| `kernel`        | Kernel core (scheduler, loader)      |
| `hal`           | HAL headers or shared HAL code       |
| `arch/x86_64`   | x86-64 architecture code             |
| `arch/aarch64`  | ARM64 architecture code              |
| `arch/riscv64`  | RISC-V architecture code             |
| `drivers/net`   | Network drivers                      |
| `drivers/storage`| Storage drivers                     |
| `drivers/input` | Input drivers                        |
| `drivers/display`| Display/framebuffer drivers         |
| `store`         | Marketplace client and verification  |
| `net`           | Network stack (TCP, DHCP, DNS)       |
| `ai`            | AI agent and bootstrap               |
| `server`        | Marketplace server                   |
| `doc`           | Documentation                        |
| `build`         | Makefile, build scripts, CI          |
| `website`       | os.aljefra.com static site           |

**Examples:**

```
drivers/storage: add UFS driver for eMMC 5.1 devices
arch/aarch64: fix GIC priority mask for IRQ 32+
hal: add hal_cache_flush() to CPU abstraction
doc: document .ajdrv signing process
build: enable -Werror for CI builds
```

---

## Areas Needing Help

We especially welcome contributions in these areas:

- **Drivers**: Audio (HDA), Bluetooth, AMD GPU, Intel GPU, additional WiFi chipsets
- **GUI Widgets**: Buttons, text input, scroll views, file browser, terminal emulator
- **Desktop Shell**: Window manager, taskbar, theme engine, settings panel
- **Documentation**: Tutorials, API reference improvements, architecture diagrams
- **Testing**: QEMU boot tests, hardware reports, regression test scripts
- **Translations**: Arabic, French, Spanish, Chinese, Hindi — for GUI strings and docs
- **Tooling**: CI/CD pipeline, automated boot tests, static analysis integration
- **Security**: Audit the Ed25519 implementation, review TLS configuration

---

## Communication

- **Bug reports**: Open a [GitHub Issue](https://github.com/QatarIT/AlJefra-OS/issues) with steps to reproduce
- **Feature requests**: Use [GitHub Discussions](https://github.com/QatarIT/AlJefra-OS/discussions) to propose ideas
- **Questions**: GitHub Discussions or open an issue tagged `question`
- **Security vulnerabilities**: Email security@qatarit.com (do not open a public issue)

---

## Code of Conduct

All contributors must follow our [Code of Conduct](CODE_OF_CONDUCT.md). We are
committed to providing a welcoming and inclusive environment for everyone.

---

## License

AlJefra OS is released under the **MIT License**. By contributing, you agree
that your contributions will be licensed under the same terms. See
[LICENSE](LICENSE) for details.

---

*AlJefra OS -- Built in Qatar. Built for the world.*
*Qatar IT -- www.QatarIT.com*
