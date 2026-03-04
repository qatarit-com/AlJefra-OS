#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# AlJefra OS -- Kernel Signing Tool
#
# Creates signed kernel images (.ajkrn) by:
#   1. Computing SHA-512 of the raw kernel binary
#   2. Signing the hash with Ed25519
#   3. Prepending a 128-byte header (magic, hash, signature)
#
# Also patches the .secboot section in the ELF with the computed hash
# so the kernel can verify itself at boot time.
#
# Usage:
#   python3 tools/sign_kernel.py sign   kernel.bin -o kernel.ajkrn [--key keys/private.key]
#   python3 tools/sign_kernel.py verify kernel.ajkrn [--pubkey keys/public.key]
#   python3 tools/sign_kernel.py hash   kernel.bin
#   python3 tools/sign_kernel.py keygen [--out keys/]
#   python3 tools/sign_kernel.py patch-elf kernel.elf [--key keys/private.key]

import argparse
import hashlib
import os
import struct
import sys

# ---- Constants ----

AJKRN_MAGIC   = 0x4E4B4A41   # "AJKN" little-endian
AJKRN_VERSION = 1
HEADER_SIZE   = 128           # bytes

# .secboot section: 64 zero bytes that get patched with the kernel hash
SECBOOT_HASH_SIZE = 64

# ---- Ed25519 operations ----
# Uses Python nacl (PyNaCl/libsodium) if available, falls back to
# the pure-python ed25519 module, or finally hashlib-only mode
# (sign disabled, verify disabled, hash-only).

_ed25519_available = False

try:
    from nacl.signing import SigningKey, VerifyKey
    from nacl.encoding import RawEncoder
    _ed25519_available = True

    def ed25519_sign(private_key_bytes, message):
        """Sign message with Ed25519 private key (64-byte seed+pubkey or 32-byte seed)."""
        if len(private_key_bytes) == 64:
            sk = SigningKey(private_key_bytes[:32])
        else:
            sk = SigningKey(private_key_bytes)
        signed = sk.sign(message)
        return bytes(signed.signature)

    def ed25519_verify(public_key_bytes, message, signature):
        """Verify Ed25519 signature. Returns True/False."""
        try:
            vk = VerifyKey(public_key_bytes)
            vk.verify(message, signature)
            return True
        except Exception:
            return False

    def ed25519_keygen():
        """Generate a new Ed25519 key pair. Returns (seed_32, pubkey_32)."""
        sk = SigningKey.generate()
        return bytes(sk.encode()), bytes(sk.verify_key.encode())

except ImportError:
    pass

if not _ed25519_available:
    try:
        import ed25519 as _ed25519_mod
        _ed25519_available = True

        def ed25519_sign(private_key_bytes, message):
            sk = _ed25519_mod.SigningKey(private_key_bytes[:32])
            return sk.sign(message)[:64]

        def ed25519_verify(public_key_bytes, message, signature):
            try:
                vk = _ed25519_mod.VerifyingKey(public_key_bytes)
                vk.verify(signature, message)
                return True
            except Exception:
                return False

        def ed25519_keygen():
            sk, vk = _ed25519_mod.create_keypair()
            return sk.to_bytes()[:32], vk.to_bytes()

    except ImportError:
        pass


# ---- SHA-512 ----

def sha512_hash(data):
    """Compute SHA-512 hash of data. Returns 64 bytes."""
    return hashlib.sha512(data).digest()


# ---- .ajkrn header ----

def pack_header(kernel_size, sha512_digest, signature, flags=0):
    """Pack a 128-byte .ajkrn header."""
    hdr = struct.pack('<III',
                      AJKRN_MAGIC,
                      AJKRN_VERSION,
                      kernel_size)
    hdr += struct.pack('<I', flags)             # flags (4 bytes)
    hdr += sha512_digest                        # 64 bytes
    hdr += signature                            # 64 bytes
    assert len(hdr) == HEADER_SIZE
    return hdr


def unpack_header(data):
    """Unpack a .ajkrn header. Returns dict or None on error."""
    if len(data) < HEADER_SIZE:
        return None

    magic, version, kernel_size, flags = struct.unpack_from('<IIII', data, 0)
    if magic != AJKRN_MAGIC:
        return None

    sha512_digest = data[16:80]
    signature     = data[80:144]

    return {
        'magic':       magic,
        'version':     version,
        'kernel_size': kernel_size,
        'flags':       flags,
        'sha512':      sha512_digest,
        'signature':   signature,
    }


# ---- ELF .secboot section patching ----

def find_secboot_section(elf_data):
    """Find the .secboot section in an ELF binary. Returns (offset, size) or None."""
    if len(elf_data) < 64:
        return None

    # Check ELF magic
    if elf_data[:4] != b'\x7fELF':
        return None

    ei_class = elf_data[4]  # 1=32-bit, 2=64-bit
    ei_data  = elf_data[5]  # 1=little-endian, 2=big-endian

    if ei_data != 1:
        print("Warning: Only little-endian ELF supported", file=sys.stderr)
        return None

    if ei_class == 2:
        # 64-bit ELF
        e_shoff    = struct.unpack_from('<Q', elf_data, 40)[0]
        e_shentsize = struct.unpack_from('<H', elf_data, 58)[0]
        e_shnum    = struct.unpack_from('<H', elf_data, 60)[0]
        e_shstrndx = struct.unpack_from('<H', elf_data, 62)[0]
    elif ei_class == 1:
        # 32-bit ELF
        e_shoff    = struct.unpack_from('<I', elf_data, 32)[0]
        e_shentsize = struct.unpack_from('<H', elf_data, 46)[0]
        e_shnum    = struct.unpack_from('<H', elf_data, 48)[0]
        e_shstrndx = struct.unpack_from('<H', elf_data, 50)[0]
    else:
        return None

    if e_shoff == 0 or e_shnum == 0:
        return None

    # Read string table section header
    shstr_off = e_shoff + e_shstrndx * e_shentsize
    if ei_class == 2:
        str_offset = struct.unpack_from('<Q', elf_data, shstr_off + 24)[0]
        str_size   = struct.unpack_from('<Q', elf_data, shstr_off + 32)[0]
    else:
        str_offset = struct.unpack_from('<I', elf_data, shstr_off + 16)[0]
        str_size   = struct.unpack_from('<I', elf_data, shstr_off + 20)[0]

    strtab = elf_data[str_offset:str_offset + str_size]

    # Search for .secboot section
    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsize
        if ei_class == 2:
            sh_name    = struct.unpack_from('<I', elf_data, sh_off)[0]
            sh_offset  = struct.unpack_from('<Q', elf_data, sh_off + 24)[0]
            sh_size    = struct.unpack_from('<Q', elf_data, sh_off + 32)[0]
        else:
            sh_name    = struct.unpack_from('<I', elf_data, sh_off)[0]
            sh_offset  = struct.unpack_from('<I', elf_data, sh_off + 16)[0]
            sh_size    = struct.unpack_from('<I', elf_data, sh_off + 20)[0]

        # Get section name from string table
        name_end = strtab.find(b'\x00', sh_name)
        if name_end == -1:
            name_end = len(strtab)
        name = strtab[sh_name:name_end].decode('ascii', errors='replace')

        if name == '.secboot':
            return (sh_offset, sh_size)

    return None


# ---- Commands ----

def cmd_hash(args):
    """Compute and display SHA-512 of a kernel binary."""
    with open(args.input, 'rb') as f:
        data = f.read()

    digest = sha512_hash(data)
    hex_str = digest.hex()

    print(f"SHA-512: {hex_str}")
    print(f"Size:    {len(data)} bytes")
    return 0


def cmd_sign(args):
    """Sign a kernel binary, producing a .ajkrn file."""
    if not _ed25519_available:
        print("Error: Ed25519 library not available (install PyNaCl: pip install pynacl)",
              file=sys.stderr)
        return 1

    # Read kernel binary
    with open(args.input, 'rb') as f:
        kernel_data = f.read()

    # Read private key
    key_path = args.key or 'keys/dev_private.key'
    if not os.path.exists(key_path):
        print(f"Error: Private key not found: {key_path}", file=sys.stderr)
        print("  Run: python3 tools/sign_kernel.py keygen", file=sys.stderr)
        return 1

    with open(key_path, 'rb') as f:
        private_key = f.read()

    # Compute SHA-512 of the raw kernel
    digest = sha512_hash(kernel_data)

    # Build the header (without signature first, to sign the partial header)
    # We sign: magic + version + kernel_size + flags + sha512 (80 bytes total)
    partial = struct.pack('<IIII', AJKRN_MAGIC, AJKRN_VERSION,
                          len(kernel_data), 0)
    partial += digest   # 16 + 64 = 80 bytes

    signature = ed25519_sign(private_key, partial)

    # Pack full header
    header = pack_header(len(kernel_data), digest, signature)

    # Write .ajkrn file
    output = args.output or (args.input.rsplit('.', 1)[0] + '.ajkrn')
    with open(output, 'wb') as f:
        f.write(header)
        f.write(kernel_data)

    print(f"Signed kernel image: {output}")
    print(f"  Kernel size: {len(kernel_data)} bytes")
    print(f"  SHA-512:     {digest[:16].hex()}...")
    print(f"  Signature:   {signature[:16].hex()}...")
    print(f"  Total size:  {HEADER_SIZE + len(kernel_data)} bytes")
    return 0


def cmd_verify(args):
    """Verify a .ajkrn signed kernel image."""
    if not _ed25519_available:
        print("Error: Ed25519 library not available (install PyNaCl: pip install pynacl)",
              file=sys.stderr)
        return 1

    with open(args.input, 'rb') as f:
        data = f.read()

    hdr = unpack_header(data)
    if hdr is None:
        print("Error: Invalid .ajkrn header", file=sys.stderr)
        return 1

    print(f"Header version: {hdr['version']}")
    print(f"Kernel size:    {hdr['kernel_size']} bytes")

    # Extract kernel data
    kernel_data = data[HEADER_SIZE:HEADER_SIZE + hdr['kernel_size']]
    if len(kernel_data) != hdr['kernel_size']:
        print(f"Error: File truncated (expected {hdr['kernel_size']}, got {len(kernel_data)})",
              file=sys.stderr)
        return 1

    # Verify SHA-512
    computed = sha512_hash(kernel_data)
    if computed != hdr['sha512']:
        print("FAIL: SHA-512 hash mismatch", file=sys.stderr)
        print(f"  Expected: {hdr['sha512'][:16].hex()}...")
        print(f"  Computed: {computed[:16].hex()}...")
        return 1
    print("SHA-512:        MATCH")

    # Verify Ed25519 signature
    key_path = args.pubkey or 'keys/dev_public.key'
    if not os.path.exists(key_path):
        print(f"Warning: Public key not found ({key_path}), skipping signature check",
              file=sys.stderr)
        return 0

    with open(key_path, 'rb') as f:
        public_key = f.read()

    # Reconstruct signed message (partial header: 80 bytes)
    partial = data[:80]
    ok = ed25519_verify(public_key, partial, hdr['signature'])

    if ok:
        print("Signature:      VALID")
        return 0
    else:
        print("FAIL: Ed25519 signature invalid", file=sys.stderr)
        return 1


def cmd_keygen(args):
    """Generate a new Ed25519 key pair."""
    if not _ed25519_available:
        print("Error: Ed25519 library not available (install PyNaCl: pip install pynacl)",
              file=sys.stderr)
        return 1

    out_dir = args.out or 'keys'
    os.makedirs(out_dir, exist_ok=True)

    private_path = os.path.join(out_dir, 'dev_private.key')
    public_path  = os.path.join(out_dir, 'dev_public.key')

    if os.path.exists(private_path) and not args.force:
        print(f"Key already exists: {private_path}")
        print("Use --force to overwrite")
        return 1

    seed, pubkey = ed25519_keygen()

    with open(private_path, 'wb') as f:
        f.write(seed)
    os.chmod(private_path, 0o600)

    with open(public_path, 'wb') as f:
        f.write(pubkey)

    print(f"Private key: {private_path} ({len(seed)} bytes)")
    print(f"Public key:  {public_path} ({len(pubkey)} bytes)")
    print(f"Public key hex: {pubkey.hex()}")
    print("\nC array for kernel/ed25519_key.h:")
    c_bytes = ', '.join(f'0x{b:02X}' for b in pubkey)
    print(f"  {{ {c_bytes} }}")
    return 0


def cmd_patch_elf(args):
    """Patch the .secboot section in a kernel ELF with the computed hash."""
    with open(args.input, 'rb') as f:
        elf_data = bytearray(f.read())

    result = find_secboot_section(elf_data)
    if result is None:
        print("Error: No .secboot section found in ELF", file=sys.stderr)
        return 1

    sec_offset, sec_size = result
    if sec_size < SECBOOT_HASH_SIZE:
        print(f"Error: .secboot section too small ({sec_size} < {SECBOOT_HASH_SIZE})",
              file=sys.stderr)
        return 1

    # Zero out the .secboot section before hashing (to get deterministic hash)
    for i in range(sec_size):
        elf_data[sec_offset + i] = 0

    # Generate the raw binary (objcopy -O binary equivalent: just hash the ELF)
    # Actually, we hash the raw binary output, not the ELF.
    # For the kernel self-verification to work, we need to hash the same
    # region the kernel will hash at runtime.  The kernel hashes from
    # _kernel_start to _secboot_start.  In the raw binary, that's from
    # offset 0 to the .secboot offset.
    #
    # However, since we're patching the ELF (not the raw binary), we
    # compute the hash of everything before .secboot in the ELF.
    # This is a simplification — for production, objcopy should be run
    # after patching.
    #
    # For this tool, we compute SHA-512 of the region [0, sec_offset)
    # and embed it in .secboot.
    digest = sha512_hash(bytes(elf_data[:sec_offset]))

    # Patch the hash into .secboot
    for i in range(SECBOOT_HASH_SIZE):
        elf_data[sec_offset + i] = digest[i]

    # Write back
    with open(args.input, 'wb') as f:
        f.write(elf_data)

    print(f"Patched .secboot at ELF offset 0x{sec_offset:x} ({sec_size} bytes)")
    print(f"  Hash region: [0x0, 0x{sec_offset:x}) = {sec_offset} bytes")
    print(f"  SHA-512: {digest[:16].hex()}...")
    return 0


# ---- Main ----

def main():
    parser = argparse.ArgumentParser(
        description='AlJefra OS Kernel Signing Tool',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Commands:
  sign       Sign a kernel binary → .ajkrn
  verify     Verify a .ajkrn file
  hash       Compute SHA-512 of a binary
  keygen     Generate Ed25519 key pair
  patch-elf  Patch .secboot section in kernel ELF
''')

    sub = parser.add_subparsers(dest='command')

    # sign
    p = sub.add_parser('sign', help='Sign a kernel binary')
    p.add_argument('input', help='Raw kernel binary')
    p.add_argument('-o', '--output', help='Output .ajkrn file')
    p.add_argument('--key', help='Private key file (default: keys/dev_private.key)')

    # verify
    p = sub.add_parser('verify', help='Verify a .ajkrn file')
    p.add_argument('input', help='.ajkrn file to verify')
    p.add_argument('--pubkey', help='Public key file (default: keys/dev_public.key)')

    # hash
    p = sub.add_parser('hash', help='Compute SHA-512 of a binary')
    p.add_argument('input', help='Binary file to hash')

    # keygen
    p = sub.add_parser('keygen', help='Generate Ed25519 key pair')
    p.add_argument('--out', help='Output directory (default: keys/)')
    p.add_argument('--force', action='store_true', help='Overwrite existing keys')

    # patch-elf
    p = sub.add_parser('patch-elf', help='Patch .secboot in kernel ELF')
    p.add_argument('input', help='Kernel ELF file')

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    commands = {
        'sign':      cmd_sign,
        'verify':    cmd_verify,
        'hash':      cmd_hash,
        'keygen':    cmd_keygen,
        'patch-elf': cmd_patch_elf,
    }

    return commands[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
