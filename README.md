# CRC Checker

A fast, modern, and feature-rich **checksum & hash utility** written in C.  
Designed for **performance**, **accuracy**, and **usability**, with SIMD acceleration, single-pass hashing, benchmarking, progress indication, and detailed system diagnostics.

---

## ✨ Features

- **Supported algorithms**
  - CRC-16 (branch-free, table-based).
  - CRC-32 (hardware-accelerated via SSE4.2).
  - CRC-64 (ECMA).
  - xxHash64.
  - xxHash128.

- **Execution modes**
  - **Normal mode** – clear output, per-hash calculation.
  - **Single-pass mode** (`-s`) – compute multiple hashes in one pass.
  - **Benchmark mode** (`--benchmark`) – throughput & timing comparison.

- **High-accuracy timing**
  - Wall-clock timing using microsecond precision.
  - Correct even for very long-running workloads.

- **User experience**
  - Live progress bar during hashing.
  - Progress bar is replaced with final hash results.
  - Colored output for readability.
  - Automatic file path resolution.

- **Debug / system info mode**
  - OS, kernel, uptime (human-readable).
  - CPU model & speed.
  - GPU name (device only).
  - RAM size.
  - Shell, terminal, username, hostname

---

## 📦 Build

### Requirements
- Linux (tested on modern distributions).
- GCC or Clang.
- CPU with **SSE4.2** support (for CRC32 acceleration).

### Compile
```bash
gcc crc.c -O3 -march=native -Wall -Wextra -msse4.2 -pthread -o crc
```
---

## 🧰 Options
### Hash selection
| Option            | Description |
| ----------------- | ----------- |
| `--crc16`, `-c16` | CRC-16      |
| `--crc64`, `-c64` | CRC-64      |
| `--x64`, `-h`     | xxHash64    |
| `--x128`, `-H`    | xxHash128   |
| `--all`, `-a`     | All hashes  |

### Modes

| Option              | Description                            |
| ------------------- | -------------------------------------- |
| `--single`, `-s`    | Single-pass mode (fast, one file scan) |
| `--benchmark`, `-b` | Benchmark all hashes                   |

---

## 📊 Benchmark Mode

Benchmark mode measures:

- Throughput (MB/s).
- Execution time (wall-clock).
### Example
```Output
CRC-32: 6EC637BF @ 8134.00 MB/s (0.006215 s)
CRC-64: 0C0C26F38D09AFBE @ 364.64 MB/s (0.138639 s)
xxH64 : 9A04426746F0D380 @ 678.18 MB/s (0.074542 s)
xxH128: 8EDA8E37839113119A04426746F0D380 @ 50552.80 MB/s (0.000001 s)
```

---

## 📈 Single-Pass Mode (`-s`)

- Computes multiple hashes simultaneously.
- Minimizes memory bandwidth.
- Shows live progress bar.
- Replaces progress bar with final results.
### Example
```bash
crc -a -s largefile.iso
```
## ⚙️ Implementation Details
## CRC-32
- Uses `_mm_crc32_u8` / `_mm_crc32_u64`.
- Hardware-accelerated when available.

### CRC-16
- Branch-free table lookup.
- Constant-time per byte.

### Timing
- Uses `gettimeofday()` for real wall-clock time.
- Avoids `clock()` inaccuracies on long runs.

### Memory
- File is memory-mapped (`mmap`) for maximum throughput

---

## 🎨 Color Coding
| Color  | Meaning            |
| ------ | ------------------ |
| Green  | Success / hashes   |
| Orange | Sizes & throughput |
| Yellow | Timing             |
| Red    | Errors             |

---

## 📄 License

Copyright © 2026 Ino Jacob. All rights reserved.

---

## 🤝 Contributing

Contributions, performance improvements, and new hash algorithms are welcome. Please submit pull requests or open issues.

## ⭐ Notes
- Designed for large files.
- Optimized for modern CPUs.
- Accurate timing even on multi-minute or multi-hour runs.
