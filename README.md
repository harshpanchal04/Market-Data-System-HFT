# HFT Market Data Distribution System

## Table of Contents
1. [System Overview](#system-overview)
2. [Architecture Components](#architecture-components)
3. [Key Differentiators & Optimizations](#key-differentiators--optimizations)
4. [Build Instructions](#build-instructions)
5. [Testing Guide](#testing-guide)
6. [Component Testing](#component-testing)
7. [Performance Verification](#performance-verification)
8. [Troubleshooting](#troubleshooting)
9. [Implementation Status](#implementation-status)

---

## System Overview

This project is a high-performance C++17 implementation of a High-Frequency Trading (HFT) Market Data Distribution System. It demonstrates the profound latency differences between standard network distribution (TCP/IP) and ultra-low-latency intra-node distribution (POSIX Shared Memory).

The system runs three distinct processes:
- **Process A (Publisher):** Generates high-frequency Level-1 market data quotes. It pushes these quotes into a wait-free ring buffer in shared memory and simultaneously broadcasts them over a TCP socket as JSON.
- **Process B (Shared-Memory Subscriber):** Attaches to the shared memory segment and polls the ring buffer. This represents the algorithmic trading engine path, achieving single-digit microsecond (or nanosecond) latencies via zero-copy data transfer.
- **Process C (TCP Subscriber):** Connects to the publisher over TCP. This represents the path for downstream analytics, risk management, or remote GUIs.

---

## Architecture Components

### 1. Core Data Structures (`TickData`)
The core transport unit is designed for maximum cache efficiency and zero-copy semantics.

```cpp
struct alignas(64) TickData {
    char     symbol[16];   // Fixed-size, null-terminated ticker
    double   bid_price;    // Best bid
    double   ask_price;    // Best ask
    int64_t  timestamp_ns; // Nanosecond-precision origin timestamp
    char     _pad[24];     // Explicit padding to exactly 64 bytes
};
```

### 2. Lock-Free Transport (`SpscQueue`)
A Single-Producer/Single-Consumer (SPSC) ring buffer that requires absolutely no locks or mutexes. It uses standard C++ `std::atomic` fences (Acquire/Release semantics) to manage concurrent reads and writes across process boundaries. Write and read indices are strictly isolated on separate cache lines to mathematically prevent false sharing.

### 3. Process Architecture Diagram

```text
┌─────────────────────────┐
│      Publisher          │
│     (Process A)         │
│  - Generates Ticks      │
│  - Binds to CPU Core 0  │
└──────┬────────────┬─────┘
       │            │
  [Zero-copy]   [TCP/JSON]
       │            │
       ▼            ▼
┌─────────────┐  ┌─────────────┐
│ SPSC Queue  │  │ TCP Socket  │
│ (Shared Mem)│  │ (Port 8800) │
└──────┬──────┘  └──────┬──────┘
       │                │
       ▼                ▼
┌─────────────┐  ┌─────────────┐
│ SHM Consumer│  │ TCP Consumer│
│ (Process B) │  │ (Process C) │
└─────────────┘  └─────────────┘
```

---

## Key Differentiators & Optimizations

This codebase is heavily optimized for the hot path:

1. **Syscall Avoidance (`MonotonicClock`)**: Fetching the time via `clock_gettime()` or `std::chrono` incurs a syscall or vDSO penalty. We use a background thread to update an atomic cached timestamp, reducing the hot-path latency footprint to a single relaxed atomic read.
2. **False Sharing Elimination**: The `SpscQueue` forces the `read_idx` and `write_idx` onto separate 64-byte boundaries. If they shared a cache line, the CPU would thrash the cache coherency protocol (MESI) across cores.
3. **Hardware Prefetching**: We utilize `__builtin_prefetch` compiler intrinsics to pull upcoming ring-buffer memory slots into the L1 cache right before the data is written.
4. **CPU Affinity**: The publisher utilizes custom wrappers (`mds::CpuPin`) to bind the thread to a specific CPU core, preventing the OS scheduler from migrating the thread and invalidating the CPU cache.
5. **TCP Tuning**: TCP sockets are explicitly configured with `TCP_NODELAY` to defeat Nagle's Algorithm, preventing the OS from batching our tiny market data ticks into larger packets.

---

## Build Instructions

### Prerequisites
- **Compiler**: C++17 compliant (GCC 8+, Clang 7+, or MSVC 2019+)
- **Build System**: CMake 3.16 or higher
- **Libraries**: Boost 1.80+ (Headers only, for Boost.ASIO)

### Build Commands
```bash
# 1. Create build directory
mkdir -p build && cd build

# 2. Configure via CMake (Defaults to Release with -O3)
cmake -DCMAKE_BUILD_TYPE=Release ..

# 3. Compile everything
make -j$(nproc)
```

### Generated Targets
- `publisher`: Market data generation and broadcasting.
- `shm_subscriber`: Shared memory consumer.
- `tcp_subscriber`: TCP network consumer.
- `test_tick_data`: Catch2 test suite.

---

## Testing Guide

The system includes a comprehensive suite of unit and property tests using the **Catch2** framework, validating cache alignment, atomics, queue wrapping, and serialization.

### Running Automated Tests
```bash
cd build
ctest --verbose
# Alternatively, run the binary directly:
./test_tick_data
```

### Manual Integration Test

1. **Terminal 1 (Publisher)**
```bash
./publisher
```
*Expected: Allocates shared memory, binds to CPU core, and starts publishing 1000 ticks/sec.*

2. **Terminal 2 (SHM Subscriber)**
```bash
./shm_subscriber
```
*Expected: Instantly connects to the shared memory segment and outputs latency statistics (usually < 2 microseconds).*

3. **Terminal 3 (TCP Subscriber)**
```bash
./tcp_subscriber
```
*Expected: Connects via localhost:8800 and prints JSON parsed quotes. Latencies will be notably higher than SHM.*

---

## Component Testing

1. **TickData Serialization**
   - Verified via Catch2 tests ensuring `to_json` and `from_json` are perfect inverses.
   - Verified 64-byte alignment and field truncation safety.

2. **Wait-Free Ring Buffer (`SpscQueue`)**
   - Verified via stress tests where a Producer thread and Consumer thread concurrently push/pop 5,000 items without locks, validating strict FIFO ordering and memory safety.

3. **Background Clock (`MonotonicClock`)**
   - Verified that cached timestamps are monotonically increasing and the background thread lifecycle behaves correctly upon process shutdown.

---

## Performance Verification

### 1. Latency Profile
- **Shared Memory Path**: By completely avoiding the kernel, typical latencies from generation to consumption should range between **0.5µs and 3.0µs** depending on hardware.
- **TCP Path**: With JSON serialization and kernel networking stacks, latencies typically range between **25µs and 150µs**.

### 2. Monitoring
- **Memory**: The shared memory segment is mapped to `/dev/shm/mds_ticks` on Linux. You can view its size using `ls -lh /dev/shm`.
- **CPU Tuning**: You can verify thread pinning on Linux using `htop` (enable custom thread columns) or `taskset -cp <PID>`.

---

## Troubleshooting

1. **Shared Memory Errors (Linux)**
   - *Error:* `shm_open failed`
   - *Fix:* Ensure you have write permissions to `/dev/shm`. You can clean up orphaned segments via `rm /dev/shm/mds_ticks`.

2. **TCP Port Collisions**
   - *Error:* `Address already in use`
   - *Fix:* The publisher binds to `8800`. Ensure no other application is using this port, or kill stranded publisher instances: `pkill -9 publisher`.

3. **Missing Boost Headers**
   - *Error:* `CMake Error: Could NOT find Boost`
   - *Fix:* Install Boost headers.
     - Ubuntu/Debian: `sudo apt install libboost-dev`
     - macOS: `brew install boost`

---

## Implementation Status

### Core Engine
- [x] 64-byte aligned `TickData` struct
- [x] Lock-free SPSC Ring Buffer
- [x] Background monotonic clock cache
- [x] CPU Affinity and Prefetch utilities

### Applications
- [x] Publisher Process (JSON serialization + SHM mapping)
- [x] SHM Subscriber Process
- [x] TCP Subscriber Process

### Testing
- [x] Catch2 Integration
- [x] Thread-stress tests for lock-free queues
- [x] JSON format validation bounds testing

---
*Developed for educational purposes to demonstrate quantitative engineering paradigms.*
