# AlJefra OS Release Process

This document describes how AlJefra OS versions are numbered, built, tested,
signed, and published.

---

## Semantic Versioning

AlJefra OS uses [Semantic Versioning](https://semver.org/):

```
MAJOR.MINOR.PATCH
```

| Component | Incremented When                                          |
|-----------|-----------------------------------------------------------|
| MAJOR     | Breaking changes to kernel API, HAL, or .ajdrv format     |
| MINOR     | New features, drivers, or architecture support (backward compatible) |
| PATCH     | Bug fixes, performance improvements, documentation updates |

Examples: `1.0.0`, `1.1.0`, `1.0.1`

---

## Branch Model

| Branch    | Purpose                                            |
|-----------|----------------------------------------------------|
| `main`    | Stable releases only. Every commit is a release tag. |
| `dev`     | Active development. PRs merge here first.          |
| `release/X.Y` | Release candidate branch (cut from dev)       |
| `hotfix/X.Y.Z` | Emergency fixes applied to main             |

### Flow

```
dev  ------>  release/1.1  ------>  main (tag v1.1.0)
                  |
              (bug fix)
                  |
              release/1.1  ------>  main (tag v1.1.1)
```

Hotfixes:

```
main  ------>  hotfix/1.0.1  ------>  main (tag v1.0.1)
                                  \-->  dev  (merge back)
```

---

## Release Checklist

Before tagging a release, complete every item:

### Build

- [ ] `make ARCH=x86_64` compiles with zero warnings (`-Wall -Werror`)
- [ ] `make ARCH=aarch64` compiles with zero warnings
- [ ] `make ARCH=riscv64` compiles with zero warnings
- [ ] Binary sizes are within expected range (x86_64 < 200 KB, etc.)

### Test

- [ ] x86-64 boots in QEMU, reaches `Ready` prompt
- [ ] ARM64 boots in QEMU, reaches `Ready` prompt
- [ ] RISC-V boots in QEMU, reaches `Ready` prompt
- [ ] PCIe device enumeration works on all architectures
- [ ] At least one storage and one network driver load successfully
- [ ] Marketplace client connects and lists drivers
- [ ] No regression from previous release (diff boot logs)

### Documentation

- [ ] `CHANGELOG.md` updated with all changes since last release
- [ ] Version string updated in kernel (`sysvar.asm` and `hal.h`)
- [ ] `ROADMAP.md` updated if milestones were reached
- [ ] Website download page updated (`website/download.html`)

### Sign and Package

- [ ] Kernel binaries signed with Ed25519 release key
- [ ] ISO image built (GRUB2 boot for x86-64)
- [ ] USB image built and tested
- [ ] SHA-256 checksums generated for all artifacts

### Publish

- [ ] Git tag created: `git tag -a v1.0.0 -m "AlJefra OS v1.0.0"`
- [ ] Tag pushed: `git push origin v1.0.0`
- [ ] GitHub Release created with changelog and binary attachments
- [ ] Artifacts uploaded to os.aljefra.com/download
- [ ] Announcement posted (GitHub Discussions, website)

---

## Changelog Format

Follow the [Keep a Changelog](https://keepachangelog.com/) format:

```markdown
## [1.1.0] - 2026-MM-DD

### Added
- New driver: Intel I225 2.5 GbE NIC
- GUI: File browser widget

### Changed
- Improved PCIe enumeration speed by 40%

### Fixed
- ARM64: GIC priority mask now handles IRQs 32-63

### Removed
- Deprecated legacy VirtIO ID support
```

Categories: **Added**, **Changed**, **Fixed**, **Deprecated**, **Removed**, **Security**

---

## Binary Signing

All release binaries are signed with the AlJefra release Ed25519 key.

### Process

1. Build the final binaries from the tagged commit
2. Generate SHA-256 checksums:
   ```bash
   sha256sum build/*/bin/*.bin > SHA256SUMS
   ```
3. Sign the checksum file:
   ```bash
   python3 server/sign_release.py --key release_key.pem --input SHA256SUMS
   ```
4. Attach `SHA256SUMS` and `SHA256SUMS.sig` to the GitHub Release

### Verification

Users can verify downloads:

```bash
sha256sum -c SHA256SUMS
python3 verify_release.py --key release_pubkey.pem --sig SHA256SUMS.sig SHA256SUMS
```

---

## Distribution Channels

| Channel              | URL                                   | Content             |
|----------------------|---------------------------------------|---------------------|
| GitHub Releases      | github.com/QatarIT/AlJefra-OS/releases | Binaries + source  |
| Website              | os.aljefra.com/download               | ISO, USB images     |
| Marketplace          | store.aljefra.com                     | .ajdrv drivers      |

---

## Hotfix Process

For critical bugs in a released version:

1. Create a `hotfix/X.Y.Z` branch from the release tag
2. Apply the minimal fix
3. Test on all affected architectures
4. Update `CHANGELOG.md` with the fix
5. Tag and release as `vX.Y.Z`
6. Merge the hotfix branch back into `dev`

Hotfixes must be small and focused. New features never go through the
hotfix process.

---

*AlJefra OS -- Built in Qatar. Built for the world.*
*Qatar IT -- www.QatarIT.com*
