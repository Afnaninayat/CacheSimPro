# CacheSimPro - Advanced Cache Simulator

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Build](https://img.shields.io/badge/Build-CMake-brightgreen.svg)
![GUI](https://img.shields.io/badge/GUI-Dear%20ImGui%20%2B%20SDL2-orange.svg)

**CacheSimPro** is a modern, high-performance graphical Cache Architecture Simulator written in **C++17** using **SDL2**, **OpenGL3**, and **Dear ImGui**. It provides real-time visualization of cache hits, misses, evictions, dirty line tracking, and detailed set/way state inspection.

---

## Key Features

- **Flexible Cache Architecture Setup**:
  - **Cache Sizes**: 256 B, 512 B, 1 KB, 2 KB, 4 KB, 8 KB, 16 KB, 64 KB (and custom power-of-2 sizes).
  - **Block Sizes**: 16 B, 32 B, 64 B, 128 B, 256 B.
  - **Associativity**: Direct-Mapped (1-Way), 2-Way, 4-Way, 8-Way, 16-Way Set Associative.
  - Strict power-of-two bitwise parameter validation.
- **Replacement Policies**:
  - **LRU** (Least Recently Used)
  - **FIFO** (First-In, First-Out)
  - **Random** (`std::uniform_int_distribution<uint32_t>`)
- **Robust Line-by-Line Memory Trace Parser**:
  - Validates memory operations (`R`, `W`, `READ`, `WRITE`, `LOAD`, `STORE`, `FETCH`).
  - Validates hexadecimal (`0x1000`) and decimal memory addresses.
  - Supports `,` and `:` separators (`R,0x1000`, `W:0x1004`).
  - Ignores blank lines and comments (`#`, `//`, `;`).
  - Reports exact line numbers for syntax errors (e.g. `Line 3: Invalid operation 'HELLO'. Use R or W.`, `Line 2: Invalid address 'XYZ'`).
- **Disk File Loading & Presets**:
  - Load custom `.txt` trace files via disk path input (`Trace File:`).
  - One-click preset buttons: `memory_trace.txt`, `trace.txt`, `trace_loop.txt`, `trace_thrash.txt`.
- **Interactive Multi-Line Trace Editor**:
  - Direct text editing with line-number gutter.
  - Quick instruction entry form (`Add Instruction`).
  - Interactive validation alerts.
- **Simulation Control Engine**:
  - **Run Sim**: Executes complete trace in batch.
  - **Step**: Step-by-step single-instruction execution.
  - **Auto Play / Pause**: Automated trace execution with configurable speed timer.
  - **Reset**: Resets cache state, line valid/dirty bits, and counters without wiping cache parameters.
- **Real-Time Visualization & Metrics**:
  - Dashboard cards: **Cache Hits**, **Cache Misses**, **Evictions**, **Hit Rate %**, and **Total Accesses**.
  - Interactive **Cache Set $\times$ Way Grid Matrix** displaying Valid bit (`V`), Tag (`Tag:0xHEX`), and hover tooltips for timestamps.
  - Cell status color coding:
    - **Emerald Green**: Active Hit line
    - **Orange / Red**: Active Eviction / Miss
    - **Electric Blue**: Valid Loaded Cache Line
  - **Execution Log Console**: Real-time access history with explicit `(Dirty eviction)` tracking.

---

## Build Prerequisites (Linux)

Ensure you have CMake, a C++17 compiler (`g++` or `clang++`), and OpenGL development headers installed:

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential cmake libgl1-mesa-dev
```

*Note: SDL2 and Dear ImGui are automatically fetched and built locally via CMake `FetchContent`.*

---

## Building and Running

```bash
# 1. Generate build files
cmake -B build -S .

# 2. Compile binary
cmake --build build -j$(nproc)

# 3. Launch CacheSimPro
./build/CacheSimPro
```

---

## Canonical Trace File Format

Memory trace files contain memory access instructions formatted as `<Operation> <Address>`:

```text
# Sample Memory Trace
R 0x1000
W 0x1004
R 0x2000
R 0x1000
W 0x2004
R 0x3000
R 0x1000
W 0x3004
R 0x4000
R 0x1004
```

---

## License

This project is open source and available under the [MIT License](LICENSE).
