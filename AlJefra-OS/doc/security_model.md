# AlJefra OS Security Model

## Overview

AlJefra OS implements a defense-in-depth security architecture spanning cryptographic
code signing, hardware-enforced memory protection, secure network transport, and crash
recovery mechanisms. Every driver loaded into the kernel must pass Ed25519 signature
verification against a trust chain rooted in a key embedded at compile time. Memory
isolation prevents code injection and privilege escalation. All external communication
uses TLS 1.3.

---

## Ed25519 Digital Signatures

### Implementation

AlJefra OS includes a complete, dependency-free implementation of Ed25519 digital
signatures as specified in RFC 8032. The implementation resides in `store/verify.c` and
comprises approximately 1,547 lines of pure C code with no external library dependencies.

Key properties of the implementation:

- **Pure C**: No assembly, no external crypto libraries. Portable across all three
  supported architectures (x86_64, aarch64, riscv64).
- **Constant-time operations**: Field arithmetic uses constant-time addition, subtraction,
  and multiplication to resist timing side-channel attacks.
- **SHA-512 hashing**: The Ed25519 scheme uses SHA-512 internally, also implemented in
  pure C within the same file.
- **Stack-only allocation**: No heap usage. All intermediate values are allocated on the
  stack to avoid memory management vulnerabilities.

### API

```c
// Verify an Ed25519 signature over a message.
// Returns 1 on success, 0 on failure.
int ed25519_verify(
    const uint8_t signature[64],   // 64-byte signature
    const uint8_t *message,        // Message bytes
    size_t message_len,            // Message length
    const uint8_t public_key[32]   // 32-byte public key
);

// Sign a message with an Ed25519 private key.
// Used by the build tools, not in kernel.
void ed25519_sign(
    uint8_t signature[64],         // Output: 64-byte signature
    const uint8_t *message,        // Message bytes
    size_t message_len,            // Message length
    const uint8_t private_key[64]  // 64-byte expanded private key
);
```

---

## Trust Chain

The AlJefra OS trust chain establishes a hierarchy of cryptographic authority from the
kernel itself down to individual driver packages.

```
+------------------------------------------+
|  AlJefra Root Key                        |
|  (embedded in kernel: ed25519_key.h)     |
|  Compiled into the kernel binary at      |
|  build time. Cannot be changed at        |
|  runtime.                                |
+------------------+-----------------------+
                   |
                   | signs
                   v
+------------------------------------------+
|  Store / Publisher Keys                  |
|  Registered with the AlJefra Foundation. |
|  Each publisher has a unique Ed25519     |
|  key pair. Public keys are distributed   |
|  via the marketplace and verified        |
|  against the root key.                   |
+------------------+-----------------------+
                   |
                   | signs
                   v
+------------------------------------------+
|  Driver Packages (.ajdrv)                |
|  Each package carries a 64-byte Ed25519  |
|  signature from the publisher.           |
|  The kernel verifies the signature       |
|  before loading any code.               |
+------------------------------------------+
```

### Root Key Embedding

The root public key is defined in `include/ed25519_key.h`:

```c
// AlJefra OS Root Verification Key
// This key is used to verify publisher keys and critical system updates.
static const uint8_t aljefra_root_pubkey[32] = {
    // 32 bytes of the Ed25519 public key
    0x..., 0x..., /* ... */
};
```

This header is included at compile time, meaning the root key becomes part of the kernel
binary. Changing the root key requires recompiling the kernel, which provides a strong
anchor for the trust chain.

---

## Package Verification Process

When the kernel receives a `.ajdrv` driver package (from the marketplace or an OTA
update), it executes the following verification sequence before loading any code:

### Step 1: Header Validation

```
Read the first 64 bytes of the package.
Verify magic == 0x56444A41 ("AJDV").
Verify format version is supported.
Verify total file size is consistent with header offsets.
```

### Step 2: Architecture Check

```
Read the arch field from the header.
Compare against the running kernel's architecture.
Reject if arch does not match (e.g., an aarch64 driver on an x86_64 kernel).
```

### Step 3: Device ID Verification

```
Read vendor_id and device_id from the header.
Verify they match the hardware device the driver claims to support.
Cross-reference against the PCI enumeration table.
```

### Step 4: Version Compatibility

```
Read min_os_major and min_os_minor from the header.
Verify the running OS version meets the minimum requirement.
Reject drivers that require a newer OS version.
```

### Step 5: Signature Extraction

```
Read signature_offset from the header.
Seek to that offset and read 64 bytes (the Ed25519 signature).
The signed data is all bytes from offset 0 to signature_offset.
```

### Step 6: Ed25519 Verification

```
Call ed25519_verify(signature, package_data, signature_offset, publisher_pubkey).
If verification returns 0 (failure), reject the package immediately.
Log the rejection reason to the kernel ring buffer.
```

### Step 7: Load

```
If all checks pass:
  - Map the code section into kernel memory with appropriate permissions.
  - Set the code pages as read-only + executable (no write).
  - Call the entry point function to initialize the driver.
```

---

## Memory Protection

AlJefra OS uses hardware memory protection features on all supported architectures to
prevent code injection, buffer overflow exploitation, and unauthorized memory access.

### Implementation

Memory protection is implemented in `kernel/memprotect.c` and the architecture-specific
MMU modules (`arch/*/mmu.c`).

### Protection Mechanisms

#### NX (No-Execute) Pages

All data pages (stack, heap, driver metadata) are mapped with the NX (No-Execute) bit
set. This prevents an attacker from injecting shellcode into data regions and executing
it.

| Architecture | NX Implementation                            |
|-------------|----------------------------------------------|
| x86_64      | NX bit in page table entries (bit 63)        |
| aarch64     | PXN/UXN bits in translation table entries    |
| riscv64     | X bit cleared in PTE for data pages          |

#### WP (Write Protect)

Kernel code pages and read-only data are mapped without write permission. The CR0.WP bit
(x86_64) ensures that even kernel-mode code cannot write to read-only pages.

#### SMEP (Supervisor Mode Execution Prevention) -- x86_64

When enabled, SMEP prevents the kernel from executing code in user-space pages. This
blocks a class of attacks where an attacker maps shellcode in user space and tricks the
kernel into jumping to it.

```c
// Enable SMEP via CR4
uint64_t cr4 = read_cr4();
cr4 |= CR4_SMEP;
write_cr4(cr4);
```

#### SMAP (Supervisor Mode Access Prevention) -- x86_64

SMAP prevents the kernel from reading or writing user-space memory unless explicitly
enabled with the STAC/CLAC instructions. This prevents confused deputy attacks.

```c
// Enable SMAP via CR4
uint64_t cr4 = read_cr4();
cr4 |= CR4_SMAP;
write_cr4(cr4);
```

#### Guard Pages

Guard pages are unmapped pages placed at the boundaries of kernel stacks and critical
data structures. Any access to a guard page triggers a page fault, catching stack
overflows and buffer overruns before they can corrupt adjacent memory.

```c
// Allocate a stack with guard pages on both ends
void *stack = memprotect_alloc_guarded(KERNEL_STACK_SIZE);
// stack layout:
//   [GUARD PAGE] [usable stack: KERNEL_STACK_SIZE] [GUARD PAGE]
```

---

## Crash Recovery

AlJefra OS includes a crash recovery system designed to capture diagnostic information
and restore the system to a working state after a fatal error.

### Panic Handler (panic.c)

When the kernel encounters an unrecoverable error, it invokes the panic handler, which
performs the following sequence:

1. **Disable interrupts**: Prevent further interrupt processing that could worsen the
   state.
2. **Register dump**: Capture and display all CPU registers at the time of the panic.
   Architecture-specific register sets are printed:
   - x86_64: RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, R8-R15, RIP, RFLAGS, CR0-CR4
   - aarch64: X0-X30, SP, PC, PSTATE, ELR_EL1, SPSR_EL1, ESR_EL1, FAR_EL1
   - riscv64: x0-x31, pc, mstatus, mcause, mtval, mepc
3. **Backtrace**: Walk the stack frame chain and print return addresses. When symbol
   information is available, function names are resolved.
4. **Kernel log flush**: Flush the kernel ring buffer (`klog`) to the console so that
   all recent log messages are visible.
5. **Auto-reboot**: After a configurable timeout (default: 10 seconds), the system
   performs a hardware reset.

### Kernel Ring Buffer (klog.c)

The kernel maintains a circular ring buffer for log messages, implemented in
`kernel/klog.c`. This buffer:

- Stores the most recent 4096 log entries.
- Supports severity levels: `KLOG_DEBUG`, `KLOG_INFO`, `KLOG_WARN`, `KLOG_ERROR`,
  `KLOG_FATAL`.
- Is written to during normal operation and flushed to the console during a panic.
- Can be read from user space via a future system call interface.

```c
void klog(int level, const char *fmt, ...);

// Example usage
klog(KLOG_INFO, "PCI device %04x:%04x detected on bus %d\n",
     vendor_id, device_id, bus);
klog(KLOG_ERROR, "Ed25519 signature verification failed for driver '%s'\n",
     driver_name);
```

---

## Network Security

### TLS 1.3 via BearSSL

All network communication between the AlJefra OS kernel and the marketplace server is
encrypted using TLS 1.3. The TLS implementation is provided by BearSSL, a small,
portable, and audited TLS library suitable for embedded and OS-level use.

Key properties:

- **TLS 1.3 only**: Older protocol versions (TLS 1.2 and below) are not supported to
  reduce attack surface.
- **Cipher suites**: AES-256-GCM with SHA-384, ChaCha20-Poly1305 with SHA-256.
- **Certificate pinning**: The marketplace server's TLS certificate is pinned in the
  kernel to prevent man-in-the-middle attacks via rogue certificate authorities.
- **No dynamic memory**: BearSSL operates with static buffers, avoiding heap allocation
  vulnerabilities.

### Connection Workflow

```
1. TCP connection to store.aljefra.com:443
2. TLS 1.3 handshake (BearSSL client)
3. Verify server certificate against pinned CA
4. Send/receive application data (JSON API, .ajdrv downloads)
5. TLS close_notify
6. TCP close
```

---

## OTA (Over-The-Air) Updates

The OTA update system allows AlJefra OS to receive and apply system updates securely.
The implementation is in `kernel/ota.c`.

### Update Process

1. **Check**: The kernel periodically queries `GET /v1/updates/{version}` to check for
   available updates.
2. **Download**: If an update is available, the signed update package is downloaded over
   TLS to a staging area in memory or disk.
3. **CRC32 integrity check**: A CRC32 checksum is computed over the downloaded package
   and compared against the expected value from the update metadata.
4. **Ed25519 verification**: The update package's Ed25519 signature is verified against
   the AlJefra root public key (not a publisher key -- OS updates use the root key
   directly).
5. **Atomic apply**: The update is applied atomically. If the system loses power during
   the update, the previous version remains intact. The update mechanism uses a two-phase
   commit:
   - Phase 1: Write new data to a staging partition/region.
   - Phase 2: Swap the active partition pointer.
6. **Reboot**: The system reboots into the updated kernel.

### Rollback

If the updated kernel fails to boot (detected by a watchdog timer), the bootloader
automatically reverts to the previous known-good kernel image.

---

## Publisher Requirements

To distribute drivers through the AlJefra Marketplace, publishers must meet the
following requirements:

1. **Registration**: Register with the AlJefra Foundation by submitting an application
   with organizational details and intended driver categories.
2. **Key generation**: Generate an Ed25519 key pair using the provided tooling
   (`ajdrv_builder.py --genkey`).
3. **Key submission**: Submit the public key to the AlJefra Foundation for signing by the
   root key. The Foundation verifies the publisher's identity before signing.
4. **Standards compliance**: Agree to and follow the AlJefra Driver Development
   Standards, which cover:
   - Code quality and review requirements.
   - Security best practices (no arbitrary memory access, proper bounds checking).
   - Testing requirements (must pass QEMU-based boot tests on all target architectures).
   - Documentation requirements (driver behavior, supported hardware, known limitations).
5. **Ongoing obligations**: Publishers must respond to security vulnerability reports
   within 72 hours and issue patches within 30 days.

---

## WiFi Security

### AES-CCMP Encryption (WPA2)

The WiFi subsystem implements AES-CCMP (Counter Mode CBC-MAC Protocol) encryption as
required by WPA2. The implementation is in `drivers/network/aes_ccmp.c`.

Key properties:

- **AES-128**: 128-bit AES block cipher in CTR mode for data encryption.
- **CBC-MAC**: Cipher Block Chaining Message Authentication Code for integrity.
- **Per-packet keys**: Derived from the Pairwise Transient Key (PTK) using the CCMP
  key derivation function.
- **Replay protection**: Packet sequence numbers are tracked to detect and reject
  replayed frames.
- **Implementation**: Pure C, no hardware AES acceleration assumed (software fallback
  for all architectures).

### WPA2 Four-Way Handshake

The WPA2 four-way handshake is implemented to establish the PTK:

1. **Message 1**: AP sends ANonce to the station.
2. **Message 2**: Station generates SNonce, computes PTK, sends SNonce + MIC.
3. **Message 3**: AP sends GTK (encrypted) + MIC.
4. **Message 4**: Station confirms installation of keys.

After the handshake, all unicast frames are encrypted with AES-CCMP using the PTK, and
all broadcast/multicast frames use the GTK.

---

## Security Boundaries Summary

| Boundary               | Mechanism                          | Implementation File     |
|------------------------|------------------------------------|-------------------------|
| Driver code signing    | Ed25519 digital signatures         | store/verify.c          |
| Trust anchor           | Root key in kernel binary          | include/ed25519_key.h   |
| Code execution         | NX pages, SMEP                     | kernel/memprotect.c     |
| Data isolation         | WP pages, SMAP, guard pages        | kernel/memprotect.c     |
| Network transport      | TLS 1.3 (BearSSL)                 | net/tls.c               |
| OTA integrity          | CRC32 + Ed25519                    | kernel/ota.c            |
| Crash diagnostics      | Panic handler + ring buffer        | kernel/panic.c, klog.c  |
| WiFi encryption        | AES-CCMP (WPA2)                   | drivers/network/aes_ccmp.c |
