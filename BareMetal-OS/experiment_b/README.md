# Experiment B: GPU Binary Evolution

**Binary-level genetic evolution of x86-64 kernel code**

## What This Does

Generates millions of binary mutations of the AlJefra OS kernel per second, scores them using an instruction latency model, validates the best candidates in QEMU, and records breakthroughs. Discovers instruction sequences no human would write.

## Prerequisites

```bash
# Required
sudo apt install nasm qemu-system-x86_64 gcc make

# Optional (GPU acceleration)
sudo apt install nvidia-cuda-toolkit
```

## Quick Start

```bash
cd BareMetal-OS/experiment_b
./evolve_binary.sh interrupts 50
```

This will:
1. Install missing dependencies
2. Build the kernel (if needed)
3. Compile the evolution engine
4. Run 50 generations of binary evolution on the `interrupts` component
5. Save results to `results/`

## How It Works

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐
│ NASM Listing │ ──→ │   Component  │ ──→ │   x86-64     │
│   Parser     │     │   Extractor  │     │   Decoder    │
└─────────────┘     └──────────────┘     └──────────────┘
                                                │
                    ┌──────────────┐            ▼
                    │  AI Guide    │     ┌──────────────┐
                    │  (per-comp)  │ ──→ │   Mutation   │
                    └──────────────┘     │   Engine     │
                                        │  (6 types)   │
                                        └──────────────┘
                                               │
                    ┌──────────────┐            ▼
                    │   QEMU       │     ┌──────────────┐
                    │   Benchmark  │ ←── │   Fitness    │
                    │   Runner     │     │  Prediction  │
                    └──────────────┘     │ (CPU/GPU)    │
                           │            └──────────────┘
                           ▼
                    ┌──────────────┐     ┌──────────────┐
                    │  Genetic     │ ──→ │ Breakthrough │
                    │  Algorithm   │     │  Recorder    │
                    └──────────────┘     └──────────────┘
```

### Pipeline Steps

1. **Parse**: NASM listing maps binary bytes to source lines
2. **Extract**: Each component's binary is isolated by function boundaries
3. **Mutate**: 6 mutation types generate candidates
   - Instruction substitution (equivalent faster encodings)
   - NOP elimination (merge/optimize padding)
   - Instruction reordering (swap independent ops)
   - Register renaming (use different registers)
   - Alignment adjustment (align branch targets)
   - Instruction fusion (enable CPU macro-fusion)
4. **Predict**: Instruction latency model scores millions of candidates
5. **Validate**: Top 10 predictions are QEMU-benchmarked (15s each)
6. **Record**: Improvements >= 5% are saved as breakthroughs
7. **Evolve**: Tournament selection + crossover → next generation

### Safety Invariants

The mutation engine NEVER breaks:
- Kernel jump table (0x100010-0x1000A8)
- Stack balance (push/pop pairs)
- IRETQ stack frames in interrupt handlers
- LOCK prefix atomicity
- Branch target validity

## Components

```
./evolve_binary.sh kernel 100      # Core kernel
./evolve_binary.sh smp 100         # Multi-core locks
./evolve_binary.sh interrupts 100  # Interrupt handlers
./evolve_binary.sh io 100          # I/O subsystem
./evolve_binary.sh scheduler 100   # AI scheduler
./evolve_binary.sh network 50      # Network stack
./evolve_binary.sh storage 50      # Storage subsystem
./evolve_binary.sh timer 50        # Timer/TSC
./evolve_binary.sh bus 50          # PCIe bus
./evolve_binary.sh gpu_driver 50   # GPU driver
./evolve_binary.sh syscalls 50     # Syscall dispatch
./evolve_binary.sh memory 50       # Memory management
```

## Results

- `results/breakthroughs.jsonl` — Breakthrough records
- `results/generations.jsonl` — Per-generation statistics
- `results/<component>_gen<N>.bin` — Evolved kernel binaries
- Git branches: `binary-evo/<component>/gen<N>`

## Comparing with Experiment A

```bash
cd ../comparison
./compare.sh
```

## Building Manually

```bash
cd experiment_b
make          # Builds evolve_bin (auto-detects CUDA)
make clean    # Clean build
make test     # Run self-test
```

## GPU Acceleration

If `nvcc` is available, the build system automatically enables CUDA:
- Fitness prediction: millions of genomes scored per second on GPU
- Mutation generation: parallel mutation on GPU

Without CUDA, OpenMP provides multi-core CPU parallelism.

## Architecture

```
experiment_b/
├── src/
│   ├── main.c              # CLI entry + evolution loop
│   ├── config.h             # All constants and types
│   ├── decoder/             # x86-64 instruction decoder
│   │   ├── x86_decode.c/h   # Instruction boundary detection
│   │   └── x86_tables.c/h   # Opcode → length/flags tables
│   ├── extractor/           # NASM listing → components
│   │   ├── listing_parser.c/h
│   │   └── component_map.c/h
│   ├── mutator/             # Binary mutation engine
│   │   ├── mutator.c/h      # 6 mutation types
│   │   └── patterns.c/h     # ~32 safe substitutions
│   ├── fitness/             # Fitness prediction
│   │   ├── fitness_cpu.c/h   # OpenMP parallel scoring
│   │   └── latency_tables.c/h # Agner Fog cycle counts
│   ├── validator/           # QEMU benchmark
│   │   ├── qemu_runner.c/h
│   │   └── serial_parser.c/h
│   ├── ga/                  # Genetic algorithm
│   │   ├── population.c/h
│   │   ├── selection.c
│   │   └── crossover.c
│   └── recorder/            # Breakthrough recording
│       └── recorder.c/h
├── cuda/                    # Optional GPU kernels
│   ├── fitness_kernel.cu
│   ├── mutation_kernel.cu
│   └── cuda_bridge.h
├── guides/                  # 15 per-component AI guides
├── results/                 # Evolution output
├── Makefile
└── evolve_binary.sh         # One-command launcher
```
