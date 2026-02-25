#!/usr/bin/env bash
# =============================================================================
# Benchmark Comparison: Baseline (fd5a707) vs Gen-8 (HEAD)
# AlJefra OS AI — Static + Dynamic Kernel Analysis
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GIT_DIR="$(cd "$REPO_ROOT/.." && git rev-parse --show-toplevel)"
REPORT="$SCRIPT_DIR/logs/BENCHMARK_BASELINE_VS_GEN8.md"

BASELINE_COMMIT="fd5a707"
GEN8_COMMIT="$(git -C "$GIT_DIR" rev-parse --short HEAD)"

# Colors for terminal output
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }

# Temp directories (cleaned on exit)
BASELINE_DIR=""
GEN8_DIR=""
cleanup() {
    [[ -n "$BASELINE_DIR" && -d "$BASELINE_DIR" ]] && rm -rf "$BASELINE_DIR"
    [[ -n "$GEN8_DIR" && -d "$GEN8_DIR" ]] && rm -rf "$GEN8_DIR"
}
trap cleanup EXIT

# ─────────────────────────────────────────────────────────────────────────────
# Verify tools
# ─────────────────────────────────────────────────────────────────────────────
command -v nasm >/dev/null 2>&1 || fail "nasm not found"
command -v git  >/dev/null 2>&1 || fail "git not found"
command -v awk  >/dev/null 2>&1 || fail "awk not found"

mkdir -p "$SCRIPT_DIR/logs"

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Extract & Build Baseline Kernel
# ─────────────────────────────────────────────────────────────────────────────
info "Step 1: Building baseline kernel ($BASELINE_COMMIT)..."
BASELINE_DIR=$(mktemp -d)
cd "$GIT_DIR"
git archive "$BASELINE_COMMIT" -- BareMetal-OS/BareMetal/ | tar -C "$BASELINE_DIR" -x

BASELINE_SRC="$BASELINE_DIR/BareMetal-OS/BareMetal/src"
BASELINE_BIN="$BASELINE_DIR/BareMetal-OS/BareMetal/bin"
mkdir -p "$BASELINE_BIN"

cd "$BASELINE_SRC"
nasm -dNO_VGA kernel.asm -o "$BASELINE_BIN/kernel.sys" -l "$BASELINE_BIN/kernel-debug.txt" \
    || fail "Baseline kernel build failed"
ok "Baseline kernel built: $(wc -c < "$BASELINE_BIN/kernel.sys") bytes"

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Build Gen-8 Kernel with Listing
# ─────────────────────────────────────────────────────────────────────────────
info "Step 2: Building Gen-8 kernel ($GEN8_COMMIT)..."
GEN8_DIR=$(mktemp -d)
cd "$GIT_DIR"
git archive HEAD -- BareMetal-OS/BareMetal/ | tar -C "$GEN8_DIR" -x

GEN8_SRC="$GEN8_DIR/BareMetal-OS/BareMetal/src"
GEN8_BIN="$GEN8_DIR/BareMetal-OS/BareMetal/bin"
mkdir -p "$GEN8_BIN"

cd "$GEN8_SRC"
nasm -dNO_VGA -dNO_GPU kernel.asm -o "$GEN8_BIN/kernel.sys" -l "$GEN8_BIN/kernel-debug.txt" \
    || fail "Gen-8 kernel build failed"
ok "Gen-8 kernel built: $(wc -c < "$GEN8_BIN/kernel.sys") bytes"

# ─────────────────────────────────────────────────────────────────────────────
# Analysis Functions
# ─────────────────────────────────────────────────────────────────────────────

# NASM listing format (ONE line number, then hex offset, then hex bytes):
#      19 00000000 E9AB000000              jmp start
#      23 0000000F 90                      align 16
#      24 00000010 [100A000000000000]      dq b_input
# Lines without code:
#       1                                  ; comment
# Include depth markers:
#       9                              <1> %include "init/64.asm"

# Count instructions in a NASM listing file
count_instructions() {
    local listing="$1"
    awk '{
        n = split($0, f)
        if (n >= 3 && f[2] ~ /^[0-9A-Fa-f]{8}$/ && f[3] ~ /^[0-9A-Fa-f]/) {
            count++
        }
    } END { print count+0 }' "$listing"
}

# Count total code bytes from listing
count_code_bytes() {
    local listing="$1"
    awk '{
        n = split($0, f)
        if (n >= 3 && f[2] ~ /^[0-9A-Fa-f]{8}$/ && f[3] ~ /^[0-9A-Fa-f]/) {
            hex = f[3]
            # Handle rep notation like "90<rep 8h>"
            if (match(hex, /^([0-9A-Fa-f]+)<rep ([0-9A-Fa-f]+)h>$/, m)) {
                total += (length(m[1]) / 2) * strtonum("0x" m[2])
            } else {
                # Strip relocation brackets [...]
                gsub(/[\[\]()]/, "", hex)
                total += length(hex) / 2
            }
        }
    } END { print int(total) }' "$listing"
}

# Count pattern occurrences in source files (grep wrapper safe under pipefail)
count_pattern() {
    local src_dir="$1"
    local pattern="$2"
    local result
    result=$(grep -rci "$pattern" "$src_dir" --include='*.asm' 2>/dev/null || true)
    echo "$result" | awk -F: '{s+=$2} END {print s+0}'
}

# Count pattern with extended regex
count_pattern_E() {
    local src_dir="$1"
    local pattern="$2"
    local result
    result=$(grep -rcE "$pattern" "$src_dir" --include='*.asm' 2>/dev/null || true)
    echo "$result" | awk -F: '{s+=$2} END {print s+0}'
}

# Count lines matching a pattern (grep -n style, safe under pipefail)
count_lines_matching() {
    local src_dir="$1"
    local pattern="$2"
    local result
    result=$(grep -rn "$pattern" "$src_dir" --include='*.asm' 2>/dev/null || true)
    echo "$result" | grep -v '^$' | wc -l
}

# Extract per-component stats from listing
# Output: component_name instruction_count code_bytes
per_component_stats() {
    local listing="$1"
    awk '
    BEGIN { current = "kernel.asm"; inst = 0; bytes = 0 }

    # Track %include directives to identify component boundaries
    /%include/ {
        # Print previous component stats
        if (inst > 0 || bytes > 0) {
            printf "%s %d %d\n", current, inst, bytes
        }
        # Extract filename from the %include "filename" directive
        if (match($0, /"([^"]+)"/, arr)) {
            current = arr[1]
        }
        inst = 0; bytes = 0
        next
    }

    # Count instruction lines: field[1]=linenum, field[2]=hex_offset(8chars), field[3]=hex_bytes
    {
        n = split($0, f)
        if (n >= 3 && f[2] ~ /^[0-9A-Fa-f]{8}$/ && f[3] ~ /^[0-9A-Fa-f]/) {
            inst++
            hex = f[3]
            if (match(hex, /^([0-9A-Fa-f]+)<rep ([0-9A-Fa-f]+)h>$/, m)) {
                bytes += (length(m[1]) / 2) * strtonum("0x" m[2])
            } else {
                gsub(/[\[\]()]/, "", hex)
                bytes += length(hex) / 2
            }
        }
    }

    END {
        if (inst > 0 || bytes > 0) {
            printf "%s %d %d\n", current, inst, bytes
        }
    }
    ' "$listing"
}

# Count source lines per .asm file
count_src_lines() {
    local src_dir="$1"
    local file="$2"
    if [[ -f "$src_dir/$file" ]]; then
        wc -l < "$src_dir/$file"
    else
        echo 0
    fi
}

# Extract a function's listing (from label to next top-level label)
extract_function() {
    local listing="$1"
    local func_label="$2"
    local func_prefix="$func_label"  # e.g. "b_smp_lock"
    awk -v label="$func_label:" -v prefix="$func_prefix" '
    BEGIN { found = 0 }
    {
        if (!found) {
            if (index($0, label) > 0) {
                line = $0
                if (match(line, /[;]/) && match(line, label) > RSTART) next
                if (match(line, "(^|[^a-zA-Z0-9_])" label, arr)) {
                    found = 1
                    print
                    next
                }
            }
            next
        }
        # Once found, collect lines until next unrelated label definition
        n = split($0, f)
        if (n >= 2) {
            src_start = 2
            if (f[src_start] ~ /^[0-9A-Fa-f]{8}$/) {
                src_start++
                if (src_start <= n && f[src_start] ~ /^[0-9A-Fa-f]/) src_start++
            }
            while (src_start <= n && f[src_start] ~ /^<[0-9]+>$/) src_start++
            if (src_start <= n) {
                src_token = f[src_start]
                if (src_token ~ /^[a-zA-Z_][a-zA-Z0-9_.]*:$/ && src_token != label) {
                    # Allow sub-labels (e.g. b_smp_lock_spin for b_smp_lock)
                    if (index(src_token, prefix) != 1) {
                        exit
                    }
                }
            }
        }
        print
    }
    ' "$listing"
}

# ─────────────────────────────────────────────────────────────────────────────
# Step 3-4: Run Analysis
# ─────────────────────────────────────────────────────────────────────────────
info "Step 3: Analyzing binary sizes..."
BASELINE_SIZE=$(wc -c < "$BASELINE_BIN/kernel.sys")
GEN8_SIZE=$(wc -c < "$GEN8_BIN/kernel.sys")
SIZE_DELTA=$((GEN8_SIZE - BASELINE_SIZE))
if [[ $BASELINE_SIZE -gt 0 ]]; then
    SIZE_PCT=$(awk "BEGIN { printf \"%.1f\", ($SIZE_DELTA / $BASELINE_SIZE) * 100 }")
else
    SIZE_PCT="N/A"
fi
ok "Binary size: baseline=${BASELINE_SIZE}B, gen8=${GEN8_SIZE}B, delta=${SIZE_DELTA}B (${SIZE_PCT}%)"

info "Step 4: Parsing NASM listings..."
BASELINE_INSTR=$(count_instructions "$BASELINE_BIN/kernel-debug.txt")
GEN8_INSTR=$(count_instructions "$GEN8_BIN/kernel-debug.txt")
INSTR_DELTA=$((GEN8_INSTR - BASELINE_INSTR))

BASELINE_CODE_BYTES=$(count_code_bytes "$BASELINE_BIN/kernel-debug.txt")
GEN8_CODE_BYTES=$(count_code_bytes "$GEN8_BIN/kernel-debug.txt")
CODE_DELTA=$((GEN8_CODE_BYTES - BASELINE_CODE_BYTES))

if [[ $BASELINE_INSTR -gt 0 ]]; then
    BASELINE_AVG_LEN=$(awk "BEGIN { printf \"%.2f\", $BASELINE_CODE_BYTES / $BASELINE_INSTR }")
    GEN8_AVG_LEN=$(awk "BEGIN { printf \"%.2f\", $GEN8_CODE_BYTES / $GEN8_INSTR }")
else
    BASELINE_AVG_LEN="N/A"
    GEN8_AVG_LEN="N/A"
fi
ok "Instructions: baseline=$BASELINE_INSTR, gen8=$GEN8_INSTR (delta=$INSTR_DELTA)"
ok "Code bytes: baseline=$BASELINE_CODE_BYTES, gen8=$GEN8_CODE_BYTES (delta=$CODE_DELTA)"
ok "Avg instr len: baseline=${BASELINE_AVG_LEN}B, gen8=${GEN8_AVG_LEN}B"

# ─────────────────────────────────────────────────────────────────────────────
# Step 5: Optimization Pattern Counts
# ─────────────────────────────────────────────────────────────────────────────
info "Step 5: Counting optimization patterns..."

BASELINE_SHARED="$BASELINE_SRC"
GEN8_SHARED="$GEN8_SRC"

declare -A PAT_BASELINE PAT_GEN8

# test (zero-test) vs cmp *, 0
PAT_BASELINE[test_instr]=$(count_pattern "$BASELINE_SHARED" 'test ')
PAT_GEN8[test_instr]=$(count_pattern "$GEN8_SHARED" 'test ')

PAT_BASELINE[cmp_zero]=$(count_lines_matching "$BASELINE_SHARED" 'cmp.*,[ ]*0$\|cmp.*,[ ]*0 ')
PAT_GEN8[cmp_zero]=$(count_lines_matching "$GEN8_SHARED" 'cmp.*,[ ]*0$\|cmp.*,[ ]*0 ')

# lea (address calculations)
PAT_BASELINE[lea]=$(count_pattern "$BASELINE_SHARED" 'lea ')
PAT_GEN8[lea]=$(count_pattern "$GEN8_SHARED" 'lea ')

# pause (spin-wait hints)
PAT_BASELINE[pause]=$(count_pattern_E "$BASELINE_SHARED" '	pause( |$)')
PAT_GEN8[pause]=$(count_pattern_E "$GEN8_SHARED" '	pause( |$)')

# bt/bts/btr (bit-test operations)
PAT_BASELINE[bt_ops]=$(count_pattern_E "$BASELINE_SHARED" '	bt[sr]? ')
PAT_GEN8[bt_ops]=$(count_pattern_E "$GEN8_SHARED" '	bt[sr]? ')

# jmp tail-calls
PAT_BASELINE[jmp]=$(count_pattern "$BASELINE_SHARED" 'jmp ')
PAT_GEN8[jmp]=$(count_pattern "$GEN8_SHARED" 'jmp ')

# prefetchnta (cache prefetch hints)
PAT_BASELINE[prefetch]=$(count_pattern_E "$BASELINE_SHARED" 'prefetch(nta|t[012])')
PAT_GEN8[prefetch]=$(count_pattern_E "$GEN8_SHARED" 'prefetch(nta|t[012])')

# xchg (implicit LOCK)
PAT_BASELINE[xchg]=$(count_pattern "$BASELINE_SHARED" 'xchg ')
PAT_GEN8[xchg]=$(count_pattern "$GEN8_SHARED" 'xchg ')

# rep stosq vs rep stosd
PAT_BASELINE[rep_stosq]=$(count_pattern "$BASELINE_SHARED" 'rep stosq')
PAT_GEN8[rep_stosq]=$(count_pattern "$GEN8_SHARED" 'rep stosq')
PAT_BASELINE[rep_stosd]=$(count_pattern "$BASELINE_SHARED" 'rep stosd')
PAT_GEN8[rep_stosd]=$(count_pattern "$GEN8_SHARED" 'rep stosd')

# call os_apic_write (replaced by inline EOI)
PAT_BASELINE[call_apic_write]=$(count_pattern "$BASELINE_SHARED" 'call os_apic_write')
PAT_GEN8[call_apic_write]=$(count_pattern "$GEN8_SHARED" 'call os_apic_write')

# Inline EOI (mov dword [rsi+0xB0], 0)
PAT_BASELINE[inline_eoi]=$(count_pattern_E "$BASELINE_SHARED" 'mov dword \[.*(0xB0|0x0B0)')
PAT_GEN8[inline_eoi]=$(count_pattern_E "$GEN8_SHARED" 'mov dword \[.*(0xB0|0x0B0)')

# movzx (zero-extend)
PAT_BASELINE[movzx]=$(count_pattern "$BASELINE_SHARED" 'movzx')
PAT_GEN8[movzx]=$(count_pattern "$GEN8_SHARED" 'movzx')

# xor eax, eax (32-bit zero idiom)
PAT_BASELINE[xor_eax]=$(count_pattern "$BASELINE_SHARED" 'xor eax')
PAT_GEN8[xor_eax]=$(count_pattern "$GEN8_SHARED" 'xor eax')

ok "Pattern counts collected"

# ─────────────────────────────────────────────────────────────────────────────
# Step 6: Per-Component Comparison
# ─────────────────────────────────────────────────────────────────────────────
info "Step 6: Per-component analysis..."

SHARED_FILES=(
    "kernel.asm"
    "init.asm"
    "init/64.asm"
    "init/bus.asm"
    "init/hid.asm"
    "init/net.asm"
    "init/nvs.asm"
    "init/sys.asm"
    "syscalls.asm"
    "syscalls/bus.asm"
    "syscalls/debug.asm"
    "syscalls/io.asm"
    "syscalls/net.asm"
    "syscalls/nvs.asm"
    "syscalls/smp.asm"
    "syscalls/system.asm"
    "drivers.asm"
    "drivers/apic.asm"
    "drivers/ioapic.asm"
    "drivers/msi.asm"
    "drivers/ps2.asm"
    "drivers/serial.asm"
    "drivers/timer.asm"
    "drivers/virtio.asm"
    "drivers/bus/pci.asm"
    "drivers/bus/pcie.asm"
    "drivers/nvs/virtio-blk.asm"
    "drivers/net/virtio-net.asm"
    "drivers/lfb/lfb.asm"
    "interrupt.asm"
    "sysvar.asm"
)

BASELINE_COMP=$(per_component_stats "$BASELINE_BIN/kernel-debug.txt")
GEN8_COMP=$(per_component_stats "$GEN8_BIN/kernel-debug.txt")

declare -A BASELINE_COMP_INSTR BASELINE_COMP_BYTES GEN8_COMP_INSTR GEN8_COMP_BYTES
while IFS=' ' read -r name instr bytes; do
    [[ -z "$name" ]] && continue
    BASELINE_COMP_INSTR["$name"]=$instr
    BASELINE_COMP_BYTES["$name"]=$bytes
done <<< "$BASELINE_COMP"
while IFS=' ' read -r name instr bytes; do
    [[ -z "$name" ]] && continue
    GEN8_COMP_INSTR["$name"]=$instr
    GEN8_COMP_BYTES["$name"]=$bytes
done <<< "$GEN8_COMP"

ok "Per-component analysis complete"

# ─────────────────────────────────────────────────────────────────────────────
# Step 7: QEMU Boot Timing (best-effort)
# ─────────────────────────────────────────────────────────────────────────────
BOOT_BASELINE_MS="N/A"
BOOT_GEN8_MS="N/A"
BOOT_DELTA_MS="N/A"

qemu_boot_time() {
    local kernel_bin="$1"
    local label="$2"
    local serial_log
    serial_log=$(mktemp)

    local pure64=""
    for candidate in "$REPO_ROOT/src/Pure64/bin/software-bios.sys" "$REPO_ROOT/Pure64/bin/software-bios.sys"; do
        if [[ -f "$candidate" ]]; then
            pure64="$candidate"
            break
        fi
    done
    if [[ -z "$pure64" ]]; then
        warn "Pure64 bootloader not found, skipping QEMU boot timing for $label" >&2
        rm -f "$serial_log"
        echo "N/A"
        return
    fi

    local boot_img
    boot_img=$(mktemp)
    dd if=/dev/zero of="$boot_img" bs=1M count=4 2>/dev/null
    dd if="$pure64" of="$boot_img" bs=512 seek=16 conv=notrunc 2>/dev/null
    dd if="$kernel_bin" of="$boot_img" bs=512 seek=48 conv=notrunc 2>/dev/null

    local start_ns end_ns elapsed_ms
    start_ns=$(date +%s%N)

    timeout 10 qemu-system-x86_64 \
        -machine q35 -smp 1 -cpu Westmere -m 256 \
        -display none -no-reboot \
        -drive file="$boot_img",format=raw,if=virtio \
        -serial file:"$serial_log" \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        2>/dev/null || true

    end_ns=$(date +%s%N)
    elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

    if [[ -s "$serial_log" ]]; then
        echo "$elapsed_ms"
    else
        echo "N/A"
    fi

    rm -f "$serial_log" "$boot_img"
}

if command -v qemu-system-x86_64 >/dev/null 2>&1; then
    info "Step 7: QEMU boot timing..."
    BOOT_BASELINE_MS=$(qemu_boot_time "$BASELINE_BIN/kernel.sys" "baseline")
    BOOT_GEN8_MS=$(qemu_boot_time "$GEN8_BIN/kernel.sys" "gen8")
    if [[ "$BOOT_BASELINE_MS" != "N/A" && "$BOOT_GEN8_MS" != "N/A" ]]; then
        BOOT_DELTA_MS=$((BOOT_GEN8_MS - BOOT_BASELINE_MS))
        ok "Boot time: baseline=${BOOT_BASELINE_MS}ms, gen8=${BOOT_GEN8_MS}ms, delta=${BOOT_DELTA_MS}ms"
    else
        warn "Boot timing incomplete (missing bootloader or failed boot)"
    fi
else
    warn "qemu-system-x86_64 not found, skipping boot timing"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 8: Hot-Path Deep Analysis
# ─────────────────────────────────────────────────────────────────────────────
info "Step 8: Hot-path analysis..."

HOT_PATHS=("ap_clear" "b_smp_lock" "b_smp_unlock" "ap_wakeup" "os_debug_dump_al")

# Count instructions and bytes in extracted function listing
func_instr_count() {
    awk '{
        n = split($0, f)
        if (n >= 3 && f[2] ~ /^[0-9A-Fa-f]{8}$/ && f[3] ~ /^[0-9A-Fa-f]/) c++
    } END { print c+0 }'
}

func_byte_count() {
    awk '{
        n = split($0, f)
        if (n >= 3 && f[2] ~ /^[0-9A-Fa-f]{8}$/ && f[3] ~ /^[0-9A-Fa-f]/) {
            hex = f[3]
            if (match(hex, /^([0-9A-Fa-f]+)<rep ([0-9A-Fa-f]+)h>$/, m)) {
                b += (length(m[1]) / 2) * strtonum("0x" m[2])
            } else {
                gsub(/[\[\]()]/, "", hex)
                b += length(hex) / 2
            }
        }
    } END { print int(b) }'
}

declare -A HP_BASELINE_INSTR HP_BASELINE_BYTES HP_GEN8_INSTR HP_GEN8_BYTES
for func in "${HOT_PATHS[@]}"; do
    base_func=$(extract_function "$BASELINE_BIN/kernel-debug.txt" "$func")
    gen8_func=$(extract_function "$GEN8_BIN/kernel-debug.txt" "$func")

    HP_BASELINE_INSTR[$func]=$(echo "$base_func" | func_instr_count)
    HP_GEN8_INSTR[$func]=$(echo "$gen8_func" | func_instr_count)
    HP_BASELINE_BYTES[$func]=$(echo "$base_func" | func_byte_count)
    HP_GEN8_BYTES[$func]=$(echo "$gen8_func" | func_byte_count)
done
ok "Hot-path analysis complete"

# ─────────────────────────────────────────────────────────────────────────────
# Step 9: Generate Markdown Report
# ─────────────────────────────────────────────────────────────────────────────
info "Step 9: Generating report..."

NOW=$(date -u '+%Y-%m-%d %H:%M UTC')
NASM_VER=$(nasm --version 2>/dev/null | head -1)

# Compute change percentages safely
pct() {
    local base=$1 delta=$2
    if [[ $base -gt 0 ]]; then
        awk "BEGIN { printf \"%.1f%%\", ($delta / $base) * 100 }"
    else
        echo "N/A"
    fi
}

cat > "$REPORT" << HEADER
# Benchmark Comparison: Baseline vs Gen-8

**Generated:** $NOW
**Tool:** benchmark_compare.sh | $NASM_VER
**Baseline:** commit \`$BASELINE_COMMIT\` — Original BareMetal OS fork
**Gen-8:** commit \`$GEN8_COMMIT\` — 8 generations of AI-directed evolution (~200+ optimizations)

> Both kernels built with \`-dNO_VGA\`. Gen-8 additionally uses \`-dNO_GPU\` to exclude
> GPU/evolution/AI code added post-fork. Comparison covers **shared original components only**.

---

## 1. Summary

| Metric | Baseline | Gen-8 | Delta | Change |
|--------|----------|-------|-------|--------|
| **Binary size** (bytes) | $BASELINE_SIZE | $GEN8_SIZE | $SIZE_DELTA | ${SIZE_PCT}% |
| **Total instructions** | $BASELINE_INSTR | $GEN8_INSTR | $INSTR_DELTA | $(pct "$BASELINE_INSTR" "$INSTR_DELTA") |
| **Code bytes** | $BASELINE_CODE_BYTES | $GEN8_CODE_BYTES | $CODE_DELTA | $(pct "$BASELINE_CODE_BYTES" "$CODE_DELTA") |
| **Avg instruction length** | ${BASELINE_AVG_LEN}B | ${GEN8_AVG_LEN}B | — | — |
| **Boot time (QEMU)** | ${BOOT_BASELINE_MS}ms | ${BOOT_GEN8_MS}ms | ${BOOT_DELTA_MS}ms | — |

HEADER

cat >> "$REPORT" << 'PATTERNS_HEADER'
---

## 2. Optimization Pattern Counts

Instruction-level patterns compared across all shared `.asm` source files.

| Pattern | Description | Baseline | Gen-8 | Delta | Direction |
|---------|-------------|----------|-------|-------|-----------|
PATTERNS_HEADER

fmt_delta() {
    local base=$1 gen8=$2
    local d=$((gen8 - base))
    if [[ $d -gt 0 ]]; then echo "+$d ↑"
    elif [[ $d -lt 0 ]]; then echo "$d ↓"
    else echo "0 —"
    fi
}

{
echo "| \`test\` | Zero-comparison (efficient) | ${PAT_BASELINE[test_instr]} | ${PAT_GEN8[test_instr]} | $(fmt_delta "${PAT_BASELINE[test_instr]}" "${PAT_GEN8[test_instr]}") |"
echo "| \`cmp *, 0\` | Zero-comparison (replaced by test) | ${PAT_BASELINE[cmp_zero]} | ${PAT_GEN8[cmp_zero]} | $(fmt_delta "${PAT_BASELINE[cmp_zero]}" "${PAT_GEN8[cmp_zero]}") |"
echo "| \`lea\` | Address calc (replaces shl+add) | ${PAT_BASELINE[lea]} | ${PAT_GEN8[lea]} | $(fmt_delta "${PAT_BASELINE[lea]}" "${PAT_GEN8[lea]}") |"
echo "| \`pause\` | Spin-wait hint (new in Gen-8) | ${PAT_BASELINE[pause]} | ${PAT_GEN8[pause]} | $(fmt_delta "${PAT_BASELINE[pause]}" "${PAT_GEN8[pause]}") |"
echo "| \`bt/bts/btr\` | Bit-test operations | ${PAT_BASELINE[bt_ops]} | ${PAT_GEN8[bt_ops]} | $(fmt_delta "${PAT_BASELINE[bt_ops]}" "${PAT_GEN8[bt_ops]}") |"
echo "| \`jmp\` | Jumps (incl. tail-call conversions) | ${PAT_BASELINE[jmp]} | ${PAT_GEN8[jmp]} | $(fmt_delta "${PAT_BASELINE[jmp]}" "${PAT_GEN8[jmp]}") |"
echo "| \`prefetchnta\` | Cache prefetch hints | ${PAT_BASELINE[prefetch]} | ${PAT_GEN8[prefetch]} | $(fmt_delta "${PAT_BASELINE[prefetch]}" "${PAT_GEN8[prefetch]}") |"
echo "| \`xchg\` | Implicit LOCK exchange (eliminated) | ${PAT_BASELINE[xchg]} | ${PAT_GEN8[xchg]} | $(fmt_delta "${PAT_BASELINE[xchg]}" "${PAT_GEN8[xchg]}") |"
echo "| \`rep stosq\` | 64-bit memory fill | ${PAT_BASELINE[rep_stosq]} | ${PAT_GEN8[rep_stosq]} | $(fmt_delta "${PAT_BASELINE[rep_stosq]}" "${PAT_GEN8[rep_stosq]}") |"
echo "| \`rep stosd\` | 32-bit memory fill (replaced) | ${PAT_BASELINE[rep_stosd]} | ${PAT_GEN8[rep_stosd]} | $(fmt_delta "${PAT_BASELINE[rep_stosd]}" "${PAT_GEN8[rep_stosd]}") |"
echo "| \`call os_apic_write\` | APIC write calls (inlined in Gen-8) | ${PAT_BASELINE[call_apic_write]} | ${PAT_GEN8[call_apic_write]} | $(fmt_delta "${PAT_BASELINE[call_apic_write]}" "${PAT_GEN8[call_apic_write]}") |"
echo "| Inline EOI | Direct APIC EOI writes | ${PAT_BASELINE[inline_eoi]} | ${PAT_GEN8[inline_eoi]} | $(fmt_delta "${PAT_BASELINE[inline_eoi]}" "${PAT_GEN8[inline_eoi]}") |"
echo "| \`movzx\` | Zero-extending loads | ${PAT_BASELINE[movzx]} | ${PAT_GEN8[movzx]} | $(fmt_delta "${PAT_BASELINE[movzx]}" "${PAT_GEN8[movzx]}") |"
echo "| \`xor eax, eax\` | 32-bit zero idiom | ${PAT_BASELINE[xor_eax]} | ${PAT_GEN8[xor_eax]} | $(fmt_delta "${PAT_BASELINE[xor_eax]}" "${PAT_GEN8[xor_eax]}") |"
} >> "$REPORT"

# Per-component comparison table
cat >> "$REPORT" << 'COMP_HEADER'

---

## 3. Per-Component Comparison

Source lines (SL), instruction count (IC), and code bytes (CB) for each shared component.

| Component | SL (base) | SL (gen8) | ΔSL | IC (base) | IC (gen8) | ΔIC | CB (base) | CB (gen8) | ΔCB |
|-----------|-----------|-----------|-----|-----------|-----------|-----|-----------|-----------|-----|
COMP_HEADER

TOTAL_BASE_SL=0; TOTAL_GEN8_SL=0
TOTAL_BASE_IC=0; TOTAL_GEN8_IC=0
TOTAL_BASE_CB=0; TOTAL_GEN8_CB=0

for file in "${SHARED_FILES[@]}"; do
    base_sl=$(count_src_lines "$BASELINE_SRC" "$file")
    gen8_sl=$(count_src_lines "$GEN8_SRC" "$file")
    d_sl=$((gen8_sl - base_sl))

    base_ic=${BASELINE_COMP_INSTR[$file]:-0}
    gen8_ic=${GEN8_COMP_INSTR[$file]:-0}
    d_ic=$((gen8_ic - base_ic))

    base_cb=${BASELINE_COMP_BYTES[$file]:-0}
    gen8_cb=${GEN8_COMP_BYTES[$file]:-0}
    d_cb=$((gen8_cb - base_cb))

    TOTAL_BASE_SL=$((TOTAL_BASE_SL + base_sl))
    TOTAL_GEN8_SL=$((TOTAL_GEN8_SL + gen8_sl))
    TOTAL_BASE_IC=$((TOTAL_BASE_IC + base_ic))
    TOTAL_GEN8_IC=$((TOTAL_GEN8_IC + gen8_ic))
    TOTAL_BASE_CB=$((TOTAL_BASE_CB + base_cb))
    TOTAL_GEN8_CB=$((TOTAL_GEN8_CB + gen8_cb))

    [[ $d_sl -gt 0 ]] && d_sl="+$d_sl"
    [[ $d_ic -gt 0 ]] && d_ic="+$d_ic"
    [[ $d_cb -gt 0 ]] && d_cb="+$d_cb"

    echo "| \`$file\` | $base_sl | $gen8_sl | $d_sl | $base_ic | $gen8_ic | $d_ic | $base_cb | $gen8_cb | $d_cb |" >> "$REPORT"
done

D_TOTAL_SL=$((TOTAL_GEN8_SL - TOTAL_BASE_SL))
D_TOTAL_IC=$((TOTAL_GEN8_IC - TOTAL_BASE_IC))
D_TOTAL_CB=$((TOTAL_GEN8_CB - TOTAL_BASE_CB))
[[ $D_TOTAL_SL -gt 0 ]] && D_TOTAL_SL="+$D_TOTAL_SL"
[[ $D_TOTAL_IC -gt 0 ]] && D_TOTAL_IC="+$D_TOTAL_IC"
[[ $D_TOTAL_CB -gt 0 ]] && D_TOTAL_CB="+$D_TOTAL_CB"
echo "| **TOTAL (shared)** | **$TOTAL_BASE_SL** | **$TOTAL_GEN8_SL** | **$D_TOTAL_SL** | **$TOTAL_BASE_IC** | **$TOTAL_GEN8_IC** | **$D_TOTAL_IC** | **$TOTAL_BASE_CB** | **$TOTAL_GEN8_CB** | **$D_TOTAL_CB** |" >> "$REPORT"

cat >> "$REPORT" << 'GEN8_ONLY_HEADER'

### Gen-8 Only Components (not in baseline)

| Component | Source Lines | Notes |
|-----------|-------------|-------|
GEN8_ONLY_HEADER

GEN8_ONLY_FILES=(
    "drivers/net/e1000.asm:Intel e1000/e1000e NIC driver"
    "syscalls/evolve.asm:Evolution benchmarking syscalls"
    "syscalls/ai_scheduler.asm:AI-powered SMP scheduler"
    "syscalls/gpu.asm:GPU syscall stubs (NO_GPU=ret)"
    "init/gpu.asm:GPU init stub (NO_GPU=ret)"
    "sysvar_gpu.asm:GPU system variables"
    "drivers/gpu/nvidia.asm:RTX 5090 driver (excluded by NO_GPU)"
)

for entry in "${GEN8_ONLY_FILES[@]}"; do
    file="${entry%%:*}"
    desc="${entry#*:}"
    if [[ -f "$GEN8_SRC/$file" ]]; then
        sl=$(wc -l < "$GEN8_SRC/$file")
    else
        sl="N/A"
    fi
    echo "| \`$file\` | $sl | $desc |" >> "$REPORT"
done

# Hot-path deep analysis
cat >> "$REPORT" << 'HP_HEADER'

---

## 4. Hot-Path Deep Analysis

Detailed comparison of the most performance-critical functions.

| Function | Instr (base) | Instr (gen8) | ΔInstr | Bytes (base) | Bytes (gen8) | ΔBytes |
|----------|-------------|-------------|--------|-------------|-------------|--------|
HP_HEADER

for func in "${HOT_PATHS[@]}"; do
    bi=${HP_BASELINE_INSTR[$func]:-0}
    gi=${HP_GEN8_INSTR[$func]:-0}
    di=$((gi - bi))
    bb=${HP_BASELINE_BYTES[$func]:-0}
    gb=${HP_GEN8_BYTES[$func]:-0}
    db=$((gb - bb))
    [[ $di -gt 0 ]] && di="+$di"
    [[ $db -gt 0 ]] && db="+$db"
    echo "| \`$func\` | $bi | $gi | $di | $bb | $gb | $db |" >> "$REPORT"
done

cat >> "$REPORT" << 'HP_DETAIL'

### Key Optimizations in Hot Paths

HP_DETAIL

for func in "${HOT_PATHS[@]}"; do
    echo "#### \`$func\`" >> "$REPORT"
    echo "" >> "$REPORT"

    # Capture into variable to avoid SIGPIPE from head under pipefail
    local_base=$(extract_function "$BASELINE_BIN/kernel-debug.txt" "$func")
    local_gen8=$(extract_function "$GEN8_BIN/kernel-debug.txt" "$func")

    echo "<details><summary>Baseline listing</summary>" >> "$REPORT"
    echo "" >> "$REPORT"
    echo '```asm' >> "$REPORT"
    echo "$local_base" | head -30 >> "$REPORT"
    echo '```' >> "$REPORT"
    echo "</details>" >> "$REPORT"
    echo "" >> "$REPORT"

    echo "<details><summary>Gen-8 listing</summary>" >> "$REPORT"
    echo "" >> "$REPORT"
    echo '```asm' >> "$REPORT"
    echo "$local_gen8" | head -30 >> "$REPORT"
    echo '```' >> "$REPORT"
    echo "</details>" >> "$REPORT"
    echo "" >> "$REPORT"
done

cat >> "$REPORT" << FOOTER

---

## 5. Evolution History

| Commit | Generation | Description |
|--------|-----------|-------------|
| \`fd5a707\` | Baseline | Original BareMetal OS fork |
| \`299d302\` | Gen-1 | 8 critical kernel optimizations |
| \`6d50acc\` | Drivers-1 | 8 driver subsystem optimizations |
| \`e7829d6\` | Gen-6 | 25 optimizations + SMP bug fix |
| \`7d0208d\` | Gen-7 | 30 optimizations (Experiment B informed) |
| \`4c32537\` | Gen-8 | 37 optimizations (inline EOI, test/cmp, LEA) |

---

## 6. Methodology

- **Static analysis** via NASM listing files (\`-l\` flag) — maps every source line to its binary encoding
- **Pattern counting** via grep across shared \`.asm\` source files
- **Per-component tracking** by parsing \`%include\` boundaries in NASM listings
- **Fair comparison**: Both kernels built with \`-dNO_VGA\`; Gen-8 with \`-dNO_GPU\` to exclude post-fork additions
- **Shared files**: ${#SHARED_FILES[@]} components present in both baseline and Gen-8
- **QEMU timing**: Wall-clock boot to serial output (if bootloader available)

---

*Report generated by \`evolution/benchmark_compare.sh\`*
FOOTER

ok "Report written to: $REPORT"
echo ""
info "════════════════════════════════════════════════"
info "  Benchmark complete!"
info "  Report: evolution/logs/BENCHMARK_BASELINE_VS_GEN8.md"
info "════════════════════════════════════════════════"
