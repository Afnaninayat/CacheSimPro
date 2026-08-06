# Interactive Parameterized Cache Simulator (C++ / Linux)

A complete, production-ready interactive Parameterized Cache Simulator written in modern **C++17** using **Dear ImGui** (SDL2 + OpenGL3 backend) on Linux.

## Features

- **Parameterized Cache Configurations**:
  - Total Cache Size (Bytes)
  - Block Size (Bytes)
  - Associativity ($n$-way set associative, direct-mapped, or fully associative)
  - Strictly enforces power-of-2 validation for all cache parameters.
- **Replacement Policies**:
  - **LRU** (Least Recently Used)
  - **FIFO** (First-In, First-Out)
  - **Random**
- **Trace Management & Parsing**:
  - Load trace files from disk or edit directly via the interactive multi-line text editor.
  - Formats supported: `R 0x1000`, `W 0x1004`, `r 1008` (ignoring blank lines and `#` / `//` comments).
- **Visualization & Real-Time Dashboard**:
  - **Statistics Dashboard**: Live tracking of Hits, Misses, Evictions, Hit Rate %, and Miss Rate %.
  - **Interactive Visualizer Grid**: Matrix of Sets $\times$ Ways displaying Valid bit ($V$) and Tag in hexadecimal.
  - **Cell Highlighting**:
    - **Green**: Active Hit line
    - **Yellow**: Active Miss (allocated into an empty line)
    - **Red**: Active Eviction (replaced valid line in a full set)
  - **Step-by-step Execution Log**: Scrollable real-time history log formatted as:
    `Address <0xHEX> -> Set <INDEX>, Tag <0xTAG> -> <HIT | MISS | MISS (Evicted Line <WAY>)>`

---

## Build Prerequisites (Linux)

Ensure you have CMake, a C++17 compliant compiler (`g++` or `clang++`), and OpenGL headers installed:

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential cmake libgl1-mesa-dev
```

*Note: SDL2 and Dear ImGui are automatically fetched and built locally via CMake `FetchContent`.*

---

## Building and Running

```bash
# 1. Generate build directory
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Compile binary
cmake --build build -j$(nproc)

# 3. Launch Cache Simulator
./build/cache_simulator
```

---

## Sample Trace File

Create a file named `trace.txt` or paste the following into the Trace Editor:

```text
# Sample Memory Trace
R 0x1000
R 0x1004
W 0x1008
R 0x2000
R 0x3000
R 0x4000
R 0x5000
R 0x1000
W 0x2004
R 0x6000
```

---

## Validation Error Handling

- Clicking **"Run Simulation"** or **"Step"** without configuring cache shows:  
  `Error: Please configure cache parameters first.`
- Clicking **"Run Simulation"** or **"Step"** with no trace loaded shows:  
  `Error: Please load a trace file first.`
- Configuring invalid non-power-of-2 parameters shows:  
  `Error: Invalid cache configuration! Size, Block, and Associativity must be powers of 2.`
