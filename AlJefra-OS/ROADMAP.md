# AlJefra OS — Production Roadmap

**Version:** 2.0 (Production Launch Plan)
**Updated:** 2026-02-26
**Owner:** Qatar IT (www.QatarIT.com)

---

## Vision

AlJefra OS is the world's first AI-native, self-evolving operating system — built in Qatar for the world. Users interact with their computer through natural language. The OS understands intent, translates it to system actions, and continuously improves itself through AI-directed evolution.

**Core principle:** No user should ever need to type a command. Talk to your OS. It handles the rest.

---

## Architecture: Two-Mode Boot

```
POWER ON
  │
  ├─ HAL Init (CPU, MMU, IRQ, Timer, Bus)
  ├─ PCIe/USB/DT Device Scan
  ├─ Built-in Driver Loading
  │
  ├─ Network Setup (DHCP → TCP/IP → Gateway)
  │
  ├─ AI Connection
  │   ├─ OFFLINE: Local SLM (Small Language Model)
  │   │   └─ Translates natural language → system commands
  │   └─ ONLINE: AlJefra AI Cloud API (full LLM)
  │       └─ Full conversation, task execution, driver download
  │
  ├─ Screen Detection
  │   ├─ NO SCREEN: Serial console + AI chat (text mode)
  │   └─ HAS SCREEN: Framebuffer detected
  │       ├─ Offer: "Download AlJefra Desktop?"
  │       ├─ Download GUI plugin from marketplace (.ajdrv)
  │       └─ Launch: Desktop with file browser + AI chat + web view
  │
  └─ SYSTEM READY
```

---

## Current Status (v1.0) — Production Audit

### What's Working
| Component | Lines | Status |
|-----------|-------|--------|
| x86-64 ASM kernel | 9,102 | PRODUCTION — boots on real hardware |
| HAL + 3 architectures | 9,797 | FUNCTIONAL — all 3 boot on QEMU |
| 22+ portable C drivers | 18,998 | FUNCTIONAL — storage, network, input, display, bus |
| TCP/IP + TLS + HTTP | 2,124 | FUNCTIONAL — real TLS 1.3 via BearSSL |
| AI agent (Claude API) | 241 | FUNCTIONAL — real API integration |
| Ed25519 crypto | 2,067 | FUNCTIONAL — full RFC 8032 implementation |
| .ajdrv driver loading | ~500 | FUNCTIONAL — framework complete |
| Marketplace server | ~800 | FUNCTIONAL — 9 Flask endpoints |
| Evolution framework | 4,555 | FUNCTIONAL — binary + source evolution |
| Website (os.aljefra.com) | 3,967 | PRODUCTION — 14 pages, responsive |

### What Needs Fixing (Gaps)
| Gap | Impact | Effort |
|-----|--------|--------|
| Framebuffer has no keyboard input path | Cannot type in GUI mode | ~200 lines |
| No in-kernel filesystem API (raw sectors only) | Cannot browse files | ~800 lines |
| Ed25519 public key is all-zeros (verification skipped) | Security bypassed | ~50 lines |
| main.c defaults to Ollama, not Claude | Wrong AI backend | ~100 lines |
| Marketplace uses plain HTTP (no TLS) | Insecure driver downloads | ~300 lines |
| Marketplace .ajdrv drivers are stubs | No real downloadable drivers | ~2,000 lines |
| OTA only checks URL, doesn't download/apply | No real updates | ~500 lines |
| 8KB buffer limit for driver downloads | Large drivers fail | ~150 lines |
| Marketplace state is in-memory | Lost on restart | ~300 lines |

---

## Release Phases

### Phase 1: Core Completion (v1.1)
**Goal:** Fix all production gaps. Everything that exists must work end-to-end.

| # | Task | Est. Lines |
|---|------|------------|
| 1.1 | Wire framebuffer console to keyboard input (PS/2 + USB HID → hal_console_getc) | ~200 |
| 1.2 | In-kernel filesystem API (fs_open, fs_read, fs_write, fs_list over BMFS) | ~800 |
| 1.3 | Embed real Ed25519 public key, enable signature verification | ~50 |
| 1.4 | Wire Claude/Anthropic API as default AI backend | ~100 |
| 1.5 | Add DHCP to netstack app (not just kernel bootstrap) | ~200 |
| 1.6 | Marketplace TLS for kernel client (reuse BearSSL) | ~300 |
| 1.7 | Real .ajdrv packages for top 5 drivers | ~2,000 |
| 1.8 | OTA: download, stage to disk, verify CRC32, apply on reboot | ~500 |
| 1.9 | Marketplace server: SQLite persistence | ~300 |
| 1.10 | Fix 8KB driver download buffer limit | ~150 |
| **Total** | | **~4,600** |

### Phase 2: AI Chat System (v1.5) — Core OS Feature
**Goal:** Natural language is the primary interface. No commands needed.

| # | Task | Description | Est. Lines |
|---|------|-------------|------------|
| 2.1 | **Chat engine** | Core message loop: input → AI → parse response → execute | ~600 |
| 2.2 | **Natural language → system commands** | AI translates "show my files" → fs_list(), "connect to wifi" → wifi_connect() | ~800 |
| 2.3 | **Command verification** | AI proposes action, user confirms before execution (safety) | ~300 |
| 2.4 | **Offline SLM** | Embedded small language model for offline command translation | ~2,000 |
| 2.5 | **Online LLM connector** | Connect to AlJefra AI Cloud or Anthropic Claude API | ~400 |
| 2.6 | **Auto-detect connectivity** | Boot → try network → online: cloud LLM, offline: local SLM | ~200 |
| 2.7 | **System action API** | Actions AI can invoke: file ops, network, settings, drivers | ~1,000 |
| 2.8 | **Context awareness** | AI knows: current files, drivers, network status, hardware | ~500 |
| 2.9 | **Conversation history** | Store chat history to disk, reload on boot | ~300 |
| 2.10 | **Multi-language** | Arabic and English as primary languages | ~200 |
| **Total** | | | **~6,300** |

**Example interactions:**
```
User: "Show me what's on the disk"
AI: Found 4 files on BMFS:
     kernel.bin    (20 KB)
     config.dat    (512 B)
     ai_agent.app  (258 KB)
     drivers.ajdrv (45 KB)

User: "Connect me to the internet"
AI: Scanning network... Found Intel e1000 NIC.
    Requesting DHCP lease... Got 192.168.1.105
    Gateway: 192.168.1.1, DNS: 8.8.8.8
    Connected. Internet is ready.

User: "Find a driver for my GPU"
AI: Detected NVIDIA RTX 4070 (10de:2786).
    Searching AlJefra Store... Found nvidia-rtx-v2.1.0.ajdrv
    Download and install? [Yes/No]

User: "ابحث عن ملفاتي"
AI: وجدت 4 ملفات على القرص:
     kernel.bin    (20 كيلوبايت)
     ...
```

### Phase 3: Desktop GUI (v2.0) — Visual Interface
**Goal:** Full graphical desktop downloaded as a plugin for users with monitors.

| # | Task | Description | Est. Lines |
|---|------|-------------|------------|
| 3.1 | **Window manager** | Tiling layout, panel system, keyboard/mouse dispatch | ~1,500 |
| 3.2 | **Widget toolkit** | Button, text input, label, list view, scroll area, panel | ~2,500 |
| 3.3 | **Mouse cursor** | Software sprite cursor, click/drag events | ~400 |
| 3.4 | **Desktop shell** | Top bar (clock, status), taskbar, app launcher | ~800 |
| 3.5 | **File browser** | BMFS file list, file info, open/delete actions | ~1,000 |
| 3.6 | **AI chat window** | Message display, text input, send button, scroll | ~1,200 |
| 3.7 | **Settings panel** | Network config, display, language, AI provider | ~800 |
| 3.8 | **Web view (minimal)** | Markdown/HTML renderer for docs and basic browsing | ~2,000 |
| 3.9 | **Terminal emulator** | For advanced users who want a command line | ~600 |
| 3.10 | **Theme engine** | Dark/light theme, color customization | ~400 |
| 3.11 | **GUI as .ajdrv plugin** | Package entire GUI as downloadable marketplace plugin | ~300 |
| **Total** | | | **~11,500** |

**Desktop Layout:**
```
┌────────────────────────────────────────────────────┐
│ ⬡ AlJefra OS    │  🌐 Connected  │  🕐 14:32  │ 🇶🇦 │
├─────────┬──────────────────────────────────────────┤
│         │                                          │
│  FILES  │          AI ASSISTANT                    │
│         │                                          │
│ 📄 kern │  You: Show me system info                │
│ 📄 conf │                                          │
│ 📦 drv  │  AI: AlJefra OS v2.0                     │
│ 📄 logs │  CPU: x86-64 (4 cores)                   │
│         │  RAM: 256 MB (210 MB free)               │
│         │  Disk: 128 MB BMFS                       │
│         │  Network: 192.168.1.105 (e1000)          │
│         │  Drivers: 8 loaded                       │
│         │                                          │
│ [+] New ├──────────────────────────────────────────┤
│         │ > Ask me anything...               [Send]│
└─────────┴──────────────────────────────────────────┘
```

### Phase 4: Production Hardening (v2.5)
**Goal:** Enterprise-grade reliability, security, and update system.

| # | Task | Description |
|---|------|-------------|
| 4.1 | **Secure boot chain** | UEFI Secure Boot → signed bootloader → signed kernel → signed drivers |
| 4.2 | **Memory protection** | Kernel/user separation, NX pages, ASLR |
| 4.3 | **Crash recovery** | Panic handler, core dump to disk, auto-reboot |
| 4.4 | **Watchdog** | Hardware watchdog for driver hangs |
| 4.5 | **Logging system** | Persistent kernel log to disk + network |
| 4.6 | **OTA delta updates** | Binary diff updates (not full image), signed + verified |
| 4.7 | **Rollback support** | A/B partition scheme, automatic rollback on failed boot |
| 4.8 | **Performance profiling** | CPU cycle counters, per-driver latency tracking |
| 4.9 | **Automated testing** | CI/CD: build → QEMU boot test → regression suite |
| 4.10 | **Fuzzing** | AFL/libFuzzer for all parsers (JSON, HTTP, DNS, ajdrv) |

### Phase 5: Community & Open Source (v3.0)
**Goal:** Open source launch, contributor ecosystem, global adoption.

| # | Task | Description |
|---|------|-------------|
| 5.1 | **Contribution guide** | CONTRIBUTING.md, code style, PR process, review guidelines |
| 5.2 | **Plugin SDK** | Documented API for third-party .ajdrv plugins |
| 5.3 | **Evolution framework (public)** | Community members run evolution experiments |
| 5.4 | **Driver bounty program** | Rewards for community-submitted drivers |
| 5.5 | **Translation system** | i18n framework, community language packs |
| 5.6 | **Hardware compatibility DB** | Community-reported working hardware list |
| 5.7 | **AlJefra Store (public)** | Public marketplace with developer accounts |
| 5.8 | **Documentation site** | Full developer docs at docs.aljefra.com |
| 5.9 | **Bug tracker** | GitHub Issues with templates, labels, milestones |
| 5.10 | **Release process** | Semantic versioning, changelogs, signed releases |

---

## AI System Architecture

```
┌─────────────────────────────────────────────┐
│              User (natural language)         │
├─────────────────────────────────────────────┤
│           AlJefra AI Router                  │
│  ┌─────────────────┬──────────────────────┐ │
│  │   OFFLINE MODE   │    ONLINE MODE       │ │
│  │                  │                      │ │
│  │  Local SLM       │  AlJefra AI Cloud    │ │
│  │  (~50 MB model)  │  (api.aljefra.com)   │ │
│  │                  │        OR             │ │
│  │  Handles:        │  Anthropic Claude     │ │
│  │  - File commands │  (api.anthropic.com)  │ │
│  │  - System info   │                      │ │
│  │  - Basic tasks   │  Handles:            │ │
│  │  - Help text     │  - Everything        │ │
│  │                  │  - Complex tasks     │ │
│  │                  │  - Code generation   │ │
│  │                  │  - Driver search     │ │
│  └─────────────────┴──────────────────────┘ │
├─────────────────────────────────────────────┤
│           System Action API                  │
│  fs_list · fs_read · fs_write · net_connect │
│  driver_load · driver_search · sys_info     │
│  display_set · wifi_scan · update_check     │
├─────────────────────────────────────────────┤
│           AlJefra OS Kernel                  │
└─────────────────────────────────────────────┘
```

### SLM (Small Language Model) — Offline Mode
- **Model:** TinyLlama 1.1B or Phi-2 2.7B (quantized to 4-bit, ~500MB–1.5GB)
- **Runtime:** llama.cpp compiled for bare metal (CPU inference, no GPU required)
- **Purpose:** Parse natural language into structured commands when offline
- **Latency:** ~2-5 seconds per response on modern x86-64
- **Storage:** Downloaded as .ajdrv plugin on first boot (if user opts in)

### LLM (Cloud) — Online Mode
- **Primary:** AlJefra AI Cloud (api.aljefra.com) — Qatar IT hosted
- **Fallback:** Anthropic Claude API (api.anthropic.com)
- **Connection:** TCP/IP → TLS 1.3 → HTTPS → REST API
- **Latency:** ~1-3 seconds (network dependent)
- **Features:** Full conversation, driver search, code generation, system diagnostics

---

## Boot Flow (Detailed)

```
1. POWER ON → Firmware loads bootloader

2. BOOTLOADER → Loads AlJefra kernel

3. HAL INIT
   ├─ CPU: control registers, caches, feature detection
   ├─ MMU: page tables, virtual memory
   ├─ Interrupts: APIC/GIC/PLIC
   ├─ Timer: HPET/Generic Timer/mtime
   └─ Console: serial + framebuffer (auto-detect)

4. DEVICE SCAN
   ├─ PCIe enumeration
   ├─ USB detection
   └─ Device tree parsing (ARM64/RISC-V)

5. DRIVER LOADING
   ├─ Match PCI vendor:device → built-in drivers
   └─ Initialize: storage, NIC, display, input

6. SCREEN DETECTION
   ├─ IF framebuffer: LFB console + boot splash
   └─ ELSE: serial console

7. NETWORK
   ├─ NIC driver ready
   ├─ DHCP lease
   ├─ TCP/IP + gateway configured
   └─ Verify connectivity

8. AI CONNECTION
   ├─ IF online: TLS → AlJefra AI Cloud → ONLINE MODE
   └─ ELSE: load SLM plugin → OFFLINE MODE

9. MARKETPLACE
   ├─ Send hardware manifest
   ├─ Download missing drivers (.ajdrv)
   └─ Check for OS updates

10. GUI OFFER (if screen)
    ├─ "AlJefra Desktop available. Download? (12 MB)"
    └─ Download → install → launch desktop

11. SYSTEM READY — AI chat active
```

---

## Hardware Support

### Tier 1 — Verified Working
| Device | Driver | Tested On |
|--------|--------|-----------|
| Intel e1000/e1000e NIC | e1000.asm + e1000.c | QEMU + real hardware |
| VirtIO Block | virtio_blk.asm + virtio_blk.c | QEMU |
| VirtIO Network | virtio_net.c | QEMU |
| PS/2 Keyboard | ps2.asm + ps2.c | QEMU + real hardware |
| Serial 16550 UART | serial.asm + console.c | All platforms |
| VGA/Framebuffer | vga.asm + lfb.c | QEMU + real hardware |

### Tier 2 — Code Complete, Needs Hardware Testing
| Device | Driver |
|--------|--------|
| NVMe SSD | nvme.c |
| AHCI/SATA | ahci.c |
| RTL8169 Ethernet | rtl8169.c |
| USB 3.0 (xHCI) | xhci.c |
| USB HID (keyboard/mouse) | usb_hid.c |
| Intel WiFi (AX200/AX210) | intel_wifi.c |
| Broadcom WiFi (RPi) | bcm_wifi.c |
| eMMC storage | emmc.c |
| Touchscreen (multi-touch) | touch.c |
| NVIDIA GPU (RTX 5090) | nvidia.asm |

### Tier 3 — Planned
| Device | Priority |
|--------|----------|
| AMD GPU (RDNA) | High |
| Intel GPU (Xe) | High |
| Audio (Intel HDA) | Medium |
| Bluetooth | Medium |
| Camera (UVC) | Low |

---

## Security Model

```
TRUST CHAIN:
  AlJefra Root CA (offline, HSM-stored)
    └─ Signs: Platform Key (embedded in kernel)
        └─ Signs: Driver Publisher Keys
            └─ Sign: Individual .ajdrv packages

VERIFICATION AT EVERY LAYER:
  Bootloader → verified by UEFI Secure Boot
  Kernel     → verified by bootloader (hash check)
  Drivers    → verified by kernel (Ed25519)
  Updates    → verified by kernel (Ed25519 + CRC32)
  AI traffic → encrypted (TLS 1.3)
```

---

## Production Size Estimates

| Phase | New Code | Cumulative |
|-------|----------|------------|
| v1.0 (current) | 55,000 lines | 55,000 |
| v1.1 (core fixes) | +4,600 | 59,600 |
| v1.5 (AI chat) | +6,300 | 65,900 |
| v2.0 (desktop GUI) | +11,500 | 77,400 |
| v2.5 (hardening) | +8,000 | 85,400 |
| v3.0 (open source) | +5,000 | 90,400 |

---

## Timeline

| Phase | Version | Target |
|-------|---------|--------|
| Core Completion | v1.1 | Q1 2026 |
| AI Chat System | v1.5 | Q2 2026 |
| Desktop GUI | v2.0 | Q3 2026 |
| Production Hardening | v2.5 | Q4 2026 |
| Open Source Launch | v3.0 | Q1 2027 |

---

## Contribution Model (Phase 5)

### For Driver Developers
1. Fork the repository
2. Create driver using Plugin SDK
3. Test with QEMU across all 3 architectures
4. Submit PR with test results
5. CI validates build + boot + signature
6. Community review (2 approvals minimum)
7. Published to AlJefra Store

### For Core Contributors
1. Pick an issue from the roadmap
2. Discuss approach in GitHub Discussions
3. Implement with tests
4. PR reviewed by maintainers
5. Merged after CI passes

### For Evolution Researchers
1. Access evolution framework documentation
2. Run experiments on local QEMU
3. Submit breakthrough candidates
4. AI validates correctness
5. Accepted breakthroughs merged to kernel

---

## Success Metrics

| Metric | Target |
|--------|--------|
| Boot to AI chat | < 10 seconds |
| Marketplace drivers | 50+ verified |
| Supported hardware | 100+ tested devices |
| Community contributors | 50+ developers |
| Architectures | 3+ (x86-64, ARM64, RISC-V) |
| Uptime | 99.9% |
| Unpatched CVEs | 0 |

---

*AlJefra OS — The First Qatari Operating System*
*Built in Qatar. Built for the world.*
*Qatar IT — www.QatarIT.com*
