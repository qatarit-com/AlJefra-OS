# AlJefra OS — Multi-Architecture Build System (Enhanced for Secure Boot)
#
# Usage:
#   make                    # Build for x86-64 (default)
#   make ARCH=aarch64       # Build for ARM64
#   make ARCH=riscv64       # Build for RISC-V 64
#   make clean              # Clean all build artifacts
#   make all-arch           # Build for all architectures

# ── Configuration ──

ARCH ?= x86_64
BUILD_DIR := build/$(ARCH)
BIN_DIR := $(BUILD_DIR)/bin

# ── Toolchain Selection ──

ifeq ($(ARCH),x86_64)
    CROSS_PREFIX ?=
    CC      = $(CROSS_PREFIX)gcc
    AS      = nasm
    LD      = $(CROSS_PREFIX)ld
    OBJCOPY = $(CROSS_PREFIX)objcopy
    ASFLAGS = -f elf64
    CFLAGS  = -m64 -march=x86-64 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx
    LDFLAGS = -T arch/x86_64/linker.ld -nostdlib
    ARCH_DIR = arch/x86_64
    KERNEL_BIN = $(BIN_DIR)/kernel_x86_64.bin
else ifeq ($(ARCH),aarch64)
    CROSS_PREFIX ?= aarch64-linux-gnu-
    CC      = $(CROSS_PREFIX)gcc
    AS      = $(CROSS_PREFIX)as
    LD      = $(CROSS_PREFIX)ld
    OBJCOPY = $(CROSS_PREFIX)objcopy
    ASFLAGS =
    CFLAGS  = -march=armv8-a -mgeneral-regs-only -mstrict-align
    LDFLAGS = -T arch/aarch64/linker.ld -nostdlib
    ARCH_DIR = arch/aarch64
    KERNEL_BIN = $(BIN_DIR)/kernel_aarch64.bin
else ifeq ($(ARCH),riscv64)
    CROSS_PREFIX ?= riscv64-linux-gnu-
    CC      = $(CROSS_PREFIX)gcc
    AS      = $(CROSS_PREFIX)as
    LD      = $(CROSS_PREFIX)ld
    OBJCOPY = $(CROSS_PREFIX)objcopy
    ASFLAGS =
    CFLAGS  = -march=rv64imafdc -mabi=lp64d -mcmodel=medany
    LDFLAGS = -T arch/riscv64/linker.ld -nostdlib
    ARCH_DIR = arch/riscv64
    KERNEL_BIN = $(BIN_DIR)/kernel_riscv64.bin
else
    $(error Unsupported architecture: $(ARCH). Use x86_64, aarch64, or riscv64)
endif

# ── Common Flags ──

CFLAGS += -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -nostdinc -nostdlib -Wall -Wextra -Werror \
          -Wno-unused-parameter -Wno-unused-function \
          -O2 -g -Ihal -I. \
          -isystem $(shell $(CC) -print-file-name=include)

ifeq ($(ARCH),x86_64)
    CFLAGS += -mcmodel=small
endif

# ── Source Files ──

ARCH_C_SRCS = $(wildcard $(ARCH_DIR)/*.c)
ARCH_S_SRCS = $(wildcard $(ARCH_DIR)/*.S)
PLATFORM_SRCS =
KERNEL_SRCS = kernel/main.c kernel/sched.c kernel/syscall.c \
              kernel/driver_loader.c kernel/ai_bootstrap.c \
              kernel/fs.c kernel/ai_chat.c kernel/keyboard.c \
              kernel/dhcp.c kernel/ota.c kernel/panic.c \
              kernel/klog.c kernel/memprotect.c kernel/secboot.c \
              kernel/shell.c

DRIVER_SRCS = $(wildcard drivers/storage/*.c) \
              $(wildcard drivers/network/*.c) \
              $(wildcard drivers/input/*.c) \
              $(wildcard drivers/display/*.c) \
              $(wildcard drivers/bus/*.c)

NET_SRCS = net/dhcp.c net/tcp.c
AI_SRCS = ai/marketplace.c
STORE_SRCS = store/verify.c store/install.c store/catalog.c
GUI_SRCS = gui/gui.c gui/widgets.c gui/desktop.c
LIB_SRCS = lib/string.c

ALL_C_SRCS = $(ARCH_C_SRCS) $(PLATFORM_SRCS) $(KERNEL_SRCS) $(DRIVER_SRCS) \
             $(NET_SRCS) $(AI_SRCS) $(STORE_SRCS) $(GUI_SRCS) $(LIB_SRCS)

# Object files mapping
ARCH_C_OBJS  = $(patsubst %.c,$(BUILD_DIR)/%.o,$(ARCH_C_SRCS))
ARCH_S_OBJS  = $(patsubst %.S,$(BUILD_DIR)/%.o,$(ARCH_S_SRCS))
PLATFORM_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(PLATFORM_SRCS))
KERNEL_OBJS  = $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS))
DRIVER_OBJS  = $(patsubst %.c,$(BUILD_DIR)/%.o,$(DRIVER_SRCS))
NET_OBJS     = $(patsubst %.c,$(BUILD_DIR)/%.o,$(NET_SRCS))
AI_OBJS      = $(patsubst %.c,$(BUILD_DIR)/%.o,$(AI_SRCS))
STORE_OBJS   = $(patsubst %.c,$(BUILD_DIR)/%.o,$(STORE_SRCS))
GUI_OBJS     = $(patsubst %.c,$(BUILD_DIR)/%.o,$(GUI_SRCS))
LIB_OBJS     = $(patsubst %.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))

ALL_OBJS = $(ARCH_S_OBJS) $(ARCH_C_OBJS) $(PLATFORM_OBJS) $(KERNEL_OBJS) \
           $(DRIVER_OBJS) $(NET_OBJS) $(AI_OBJS) $(STORE_OBJS) $(GUI_OBJS) $(LIB_OBJS)

# ── Targets ──

.PHONY: all clean all-arch info check-docs sign verify keygen patch-elf gen-roadmap docs \
       iso test-bios test-uefi test-boot

all: $(KERNEL_BIN)
	@echo "Built $(KERNEL_BIN) for $(ARCH)"
	@ls -lh $(KERNEL_BIN)

$(KERNEL_BIN): $(ALL_OBJS) $(ARCH_DIR)/linker.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $(BUILD_DIR)/kernel.elf $(ALL_OBJS)
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel.elf $@

$(BUILD_DIR)/%.o: %.c | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S | dirs
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

dirs:
	@mkdir -p $(BUILD_DIR)/$(ARCH_DIR)
	@mkdir -p $(BUILD_DIR)/src/aljefra/api
	@mkdir -p $(BUILD_DIR)/kernel
	@mkdir -p $(BUILD_DIR)/drivers/storage
	@mkdir -p $(BUILD_DIR)/drivers/network
	@mkdir -p $(BUILD_DIR)/drivers/input
	@mkdir -p $(BUILD_DIR)/drivers/display
	@mkdir -p $(BUILD_DIR)/drivers/bus
	@mkdir -p $(BUILD_DIR)/net
	@mkdir -p $(BUILD_DIR)/ai
	@mkdir -p $(BUILD_DIR)/store
	@mkdir -p $(BUILD_DIR)/gui
	@mkdir -p $(BUILD_DIR)/lib

$(BIN_DIR):
	@mkdir -p $@

all-arch:
	$(MAKE) ARCH=x86_64
	$(MAKE) ARCH=aarch64
	$(MAKE) ARCH=riscv64

clean:
	rm -rf build/

check-docs:
	@python3 tools/doc_check.py

gen-roadmap:
	@python3 tools/gen_roadmap.py

docs:
	@echo "=== Regenerating roadmap.html from ROADMAP.md ==="
	@python3 tools/gen_roadmap.py
	@echo "=== Checking all documentation for consistency ==="
	@python3 tools/doc_check.py --fix --verbose
	@echo "=== Documentation pipeline complete ==="

# ── ISO & Boot Testing (x86_64 only) ──

ISO_STAGING := /tmp/aljefra_iso_build
ISO_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
ISO_OUT     := website/aljefra_os_v$(ISO_VERSION).iso

iso: $(KERNEL_BIN)
	@echo "=== Building ISO v$(ISO_VERSION) ==="
	@rm -rf $(ISO_STAGING)
	@mkdir -p $(ISO_STAGING)/boot/grub/fonts
	@cp $(KERNEL_BIN) $(ISO_STAGING)/boot/aljefra.bin
	@cp boot/grub.cfg $(ISO_STAGING)/boot/grub/grub.cfg
	@for f in /usr/share/grub/unicode.pf2 /usr/share/grub2/unicode.pf2; do \
		[ -f "$$f" ] && cp "$$f" $(ISO_STAGING)/boot/grub/fonts/ && break; \
	done; true
	grub-mkrescue -o $(ISO_OUT) $(ISO_STAGING) 2>&1 | tail -1
	@ls -lh $(ISO_OUT)

test-bios: $(KERNEL_BIN)
	@echo "=== BIOS Boot Test (QEMU) ==="
	@rm -f /tmp/aljefra_test_bios.log
	@timeout 15s qemu-system-x86_64 \
		-kernel $(KERNEL_BIN) \
		-serial file:/tmp/aljefra_test_bios.log \
		-display none -no-reboot -m 128M 2>/dev/null || true
	@grep -q "Console initialized" /tmp/aljefra_test_bios.log

test-uefi: iso
	@echo "=== UEFI Boot Test (QEMU + OVMF) ==="
	@rm -f /tmp/aljefra_test_uefi.log /tmp/aljefra_test_vars.fd
	@cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/aljefra_test_vars.fd
	@timeout 25s qemu-system-x86_64 -machine q35 \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/tmp/aljefra_test_vars.fd \
		-cdrom $(ISO_OUT) \
		-serial file:/tmp/aljefra_test_uefi.log \
		-display none -no-reboot -m 256M -boot d 2>/dev/null || true

test-boot: test-bios test-uefi

# ── Secure Boot Signing (Fixed for 144-byte Headers) ──

patch-elf: $(KERNEL_BIN)
	python3 tools/sign_kernel.py patch-elf $(BUILD_DIR)/kernel.elf

sign: $(KERNEL_BIN)
	@echo "Signing kernel for $(ARCH) with AlJefra Secure Boot..."
	python3 tools/sign_kernel.py sign $(KERNEL_BIN) \
		-o $(BIN_DIR)/kernel_$(ARCH).ajkrn

verify:
	python3 tools/sign_kernel.py verify $(BIN_DIR)/kernel_$(ARCH).ajkrn

keygen:
	python3 tools/sign_kernel.py keygen --out keys/

info:
	@echo "Architecture: $(ARCH)"
	@echo "Compiler:     $(CC)"
	@echo "Kernel Binary: $(KERNEL_BIN)"
