# AlJefra OS Hardware Compatibility Database

This document tracks hardware device support across AlJefra OS. Devices are
classified into three tiers based on their testing and verification status.

---

## Tier System

| Tier   | Status          | Description                                          |
|--------|-----------------|------------------------------------------------------|
| Tier 1 | **Verified**    | Driver complete and tested on real or emulated HW    |
| Tier 2 | **Code Complete** | Driver written and compiles, awaiting HW testing   |
| Tier 3 | **Planned**     | No driver yet; on the roadmap                        |

---

## Tier 1 -- Verified

These devices have been tested and confirmed working in QEMU or on real
hardware.

| Device             | Driver File                 | Architecture    | Tester        | Notes                              |
|--------------------|-----------------------------|-----------------|---------------|------------------------------------|
| Intel e1000/e1000e | `drivers/net/e1000.asm`     | x86-64          | AlJefra Team  | QEMU 82540EM, 82574L, I217, I219  |
| VirtIO-Blk         | `drivers/storage/virtio_blk.c` | x86-64, ARM64, RISC-V | AlJefra Team | QEMU virt machine, legacy + modern |
| VirtIO-Net         | `drivers/net/virtio_net.c`  | x86-64, ARM64, RISC-V | AlJefra Team | QEMU virt machine                  |
| PS/2 Keyboard      | `drivers/input/ps2.c`       | x86-64          | AlJefra Team  | QEMU i8042 controller              |
| Serial UART        | `drivers/input/serial.c`    | x86-64, ARM64, RISC-V | AlJefra Team | 16550A compatible, QEMU stdio    |
| VGA / Linear FB    | `drivers/display/framebuffer.c` | x86-64      | AlJefra Team  | QEMU std VGA, linear framebuffer   |

---

## Tier 2 -- Code Complete

Driver code exists and compiles cleanly but has not been tested on physical
hardware. Community testers are welcome.

| Device               | Driver File                    | Architecture    | Notes                                |
|----------------------|--------------------------------|-----------------|--------------------------------------|
| NVMe                 | `drivers/storage/nvme.c`       | x86-64, ARM64   | Awaiting real NVMe SSD test          |
| AHCI (SATA)          | `drivers/storage/ahci.c`       | x86-64          | Awaiting SATA disk test              |
| RTL8169 GbE          | `drivers/net/rtl8169.c`        | x86-64          | Realtek RTL8169/8168/8111            |
| xHCI USB 3.0         | `drivers/input/xhci.c`        | x86-64          | Host controller only, basic HID      |
| USB HID              | `drivers/input/usb_hid.c`     | x86-64          | Keyboard and mouse via xHCI          |
| Intel WiFi AX200/210 | `drivers/net/intel_wifi.c`    | x86-64          | Firmware load not yet implemented    |
| BCM WiFi (RPi)       | `drivers/net/bcm_wifi.c`      | ARM64           | Broadcom 43455, RPi 3B+/4           |
| eMMC                 | `drivers/storage/emmc.c`       | ARM64           | Speed tuning implemented             |
| UFS                  | `drivers/storage/ufs.c`        | ARM64           | UFS 2.1 protocol                     |
| Touchscreen          | `drivers/input/touchscreen.c`  | ARM64           | Generic I2C/SPI touch panel          |
| NVIDIA GPU           | `drivers/gpu/nvidia.asm`       | x86-64          | RTX 5090, compute + display          |

---

## Tier 3 -- Planned

No driver code exists yet. Contributions welcome.

| Device            | Architecture    | Priority | Notes                                  |
|-------------------|-----------------|----------|----------------------------------------|
| AMD GPU (RDNA 3+) | x86-64         | High     | Display + compute support needed       |
| Intel GPU (Xe)    | x86-64          | High     | Integrated graphics, very common       |
| Audio HDA         | x86-64          | Medium   | Intel High Definition Audio            |
| Bluetooth         | x86-64, ARM64   | Medium   | HCI transport over USB or UART         |
| Intel I225/I226   | x86-64          | Medium   | 2.5 GbE NIC                           |
| Qualcomm WiFi     | ARM64           | Low      | Ath11k/Ath12k chipsets                 |
| RISC-V SiFive     | RISC-V          | Low      | SiFive FU740 peripherals              |
| TPM 2.0           | x86-64          | Low      | Trusted Platform Module                |

---

## QEMU Tested Configurations

These QEMU invocations have been verified to boot successfully.

### x86-64

```bash
qemu-system-x86_64 -machine q35 -cpu Westmere -m 256 -smp 1 \
  -serial stdio -display none \
  -kernel build/x86_64/bin/kernel_x86_64.bin \
  -device virtio-blk-pci,drive=hd0 \
  -drive file=test.img,format=raw,id=hd0,if=none \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0
```

### ARM64

```bash
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 256 \
  -serial stdio -display none \
  -kernel build/aarch64/bin/kernel_aarch64.bin \
  -device virtio-blk-device,drive=hd0 \
  -drive file=test.img,format=raw,id=hd0,if=none \
  -device virtio-net-device,netdev=net0 \
  -netdev user,id=net0
```

### RISC-V 64

```bash
qemu-system-riscv64 -M virt -m 256 \
  -serial stdio -display none \
  -kernel build/riscv64/bin/kernel_riscv64.bin \
  -device virtio-blk-device,drive=hd0 \
  -drive file=test.img,format=raw,id=hd0,if=none \
  -device virtio-net-device,netdev=net0 \
  -netdev user,id=net0
```

---

## How to Add Hardware

If you have tested AlJefra OS on hardware not listed above, please help
expand this database.

### Testing Process

1. Boot AlJefra OS on the target hardware or QEMU configuration
2. Observe the boot log for device detection and driver binding
3. Verify the device works (e.g., read/write for storage, ping for network)
4. Record the output and any issues

### Reporting Template

Open a GitHub issue or pull request with the following information:

```
## Hardware Test Report

**Device:** [Manufacturer and model]
**PCI Vendor/Device ID:** [e.g., 8086:1533]
**Architecture:** [x86-64 / ARM64 / RISC-V]
**Driver used:** [Built-in or .ajdrv name]
**Tier:** [1 = working, 2 = loads but untested, 3 = not supported]
**Test environment:** [Real hardware / QEMU version]

### Boot log (relevant lines)
```
[paste relevant boot log output]
```

### Functionality tested
- [ ] Device detected during bus scan
- [ ] Driver loaded without errors
- [ ] Basic I/O works (read/write/ping)
- [ ] Performance acceptable

### Issues
[Describe any problems encountered]
```

---

## Known Issues

| Device        | Issue                                       | Status     |
|---------------|---------------------------------------------|------------|
| Intel WiFi    | Firmware loading not implemented             | Blocked    |
| NVIDIA GPU    | Requires real PCIe passthrough for testing   | Workaround |
| eMMC          | Speed tuning may not work on all SoCs        | Open       |
| xHCI          | Only basic HID devices supported             | Open       |

---

*AlJefra OS -- Built in Qatar. Built for the world.*
*Qatar IT -- www.QatarIT.com*
