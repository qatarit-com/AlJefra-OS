# Changelog

All notable changes to AlJefra OS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.0.0] - 2026-02-27

### Added

- **Core Kernel**: x86-64 assembly kernel (20 KB, 9,102 lines of ASM)
- **Multi-Architecture Support**: Boots on x86-64, ARM64 (Cortex-A72), and
  RISC-V 64-bit (OpenSBI) -- all verified in QEMU
- **Hardware Abstraction Layer**: 9 HAL headers (cpu, interrupt, timer, bus,
  io, mmu, smp, console, hal) enabling portable driver development
- **22+ Portable C Drivers**: NVMe, AHCI, VirtIO-Blk, VirtIO-Net, eMMC, UFS,
  e1000, RTL8169, Intel WiFi (AX200/AX210), BCM WiFi (RPi), xHCI USB 3.0,
  USB HID, PS/2, touchscreen, serial console, framebuffer, PCIe, Device Tree
  parser, ACPI lite, WiFi framework with AES-CCMP
- **Full Networking Stack**: TCP/IP (ARP, IPv4, ICMP, UDP, TCP client), DNS
  resolver, DHCP client, TLS 1.3 via BearSSL, HTTP/1.1 client with chunked
  transfer encoding
- **Driver Marketplace**: Flask REST API with 9 endpoints for discovering,
  downloading, reviewing, and updating `.ajdrv` driver packages
- **Runtime Driver Loading**: `.ajdrv` format (header + metadata JSON + PIC
  binary + Ed25519 signature), downloaded from marketplace and loaded at
  runtime without reboot
- **Ed25519 Verification**: Full RFC 8032 implementation (1,550 lines of C,
  SHA-512 + GF(2^255-19)) for cryptographic driver signing
- **AI Bootstrap Agent**: Claude API integration for hardware detection and
  driver recommendation (POST /v1/messages)
- **AI Evolution Framework**: Dual experiment system -- AI-directed source
  optimization (Experiment A) and GPU-accelerated binary evolution
  (Experiment B) with breakthrough auto-recording
- **Self-Evolving Kernel**: 11 generations of evolution with 200+ verified
  optimizations across 25 kernel ASM files
- **SMP Scheduler**: Multi-core scheduling with AI-powered task placement
- **GPU Support**: NVIDIA RTX 5090 driver (951 lines ASM) with compute and
  display capabilities
- **OTA Update System**: Marketplace update checking, staging header, CRC32
  verification, storage staging
- **Website**: Static site for os.aljefra.com (20 files, 324 KB)
- **Bootable USB Image**: GRUB2 ISO (2.3 MB) for real hardware boot

### Build Sizes

| Architecture | Kernel Binary |
|-------------|---------------|
| x86-64      | 147 KB        |
| ARM64       | 153 KB        |
| RISC-V 64   | 129 KB        |

---

*AlJefra OS v1.0.0 -- The First Qatari Operating System*
*Built in Qatar. Built for the world.*
*Qatar IT -- www.QatarIT.com*
