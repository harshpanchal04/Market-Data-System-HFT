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
- **Process B (Shared-Memory Subscriber):** Attaches to the shared memory segment and busy-polls the ring buffer. This represents the algorithmic trading engine path: no syscall and no copy between address spaces per message (median ~0.25 µs measured, see Performance).
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

1. **Two clocks for two jobs**: `MonotonicClock` caches `steady_clock` in an atomic that a background thread refreshes every 1 ms, so reading it costs one relaxed atomic load. It is only 1 ms accurate, which is fine for coarse timestamps and useless for per-message latency. Ticks are therefore stamped with `mds::precise_now_ns()` (`CLOCK_MONOTONIC` through the vDSO: no syscall, tens of nanoseconds, the same clock in every process).
2. **False Sharing Elimination**: The `SpscQueue` forces the `read_idx` and `write_idx` onto separate 64-byte boundaries. If they shared a cache line, the CPU would thrash the cache coherency protocol (MESI) across cores.
3. **Hardware Prefetching**: We utilize `__builtin_prefetch` compiler intrinsics to pull upcoming ring-buffer memory slots into the L1 cache right before the data is written.
4. **CPU Affinity**: The publisher binds to core 0 and the shared-memory subscriber to core 1 (`mds::CpuPin`), so the scheduler does not migrate them and the ring's cache lines move between two fixed cores.
6. **Busy polling**: The shared-memory subscriber spins on `pop()` with a `pause`/`yield` hint and never sleeps. Sleeping after N empty polls would add the OS wake-up delay (tens of microseconds) to any tick that arrives during the sleep.
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

All three binaries take optional positional arguments:

| Binary | Arguments (defaults) |
|---|---|
| `publisher` | `ticks` (1000), `interval_us` (1000), `startup_delay_ms` (0) |
| `shm_subscriber` | `count` (1000), `warmup` (0) |
| `tcp_subscriber` | `count` (50), `warmup` (0) |

1. **Terminal 1:** `./publisher 20000 100 5000` creates the segment and the TCP server, waits 5 s for subscribers, then sends 20,000 ticks, one every 100 µs.
2. **Terminal 2:** `./shm_subscriber 20000 1000` attaches, busy-polls, and prints p50/p90/p99/p99.9 after dropping 1,000 warm-up samples.
3. **Terminal 3:** `./tcp_subscriber 20000 1000` does the same over TCP + JSON.

Or run all three with one command: `scripts/bench.sh build 20000 100`. It prints one `RESULT` line per path.

---

## Component Testing

1. **TickData Serialization**
   - Verified via Catch2 tests ensuring `to_json` and `from_json` are perfect inverses.
   - Verified 64-byte alignment and field truncation safety.

2. **Wait-Free Ring Buffer (`SpscQueue`)**
   - Verified via stress tests where a Producer thread and Consumer thread concurrently push/pop 5,000 items without locks, validating strict FIFO ordering and memory safety.

3. **Clocks**
   - `MonotonicClock`: cached timestamps never decrease, and the background thread shuts down cleanly.
   - `precise_now_ns()`: never decreases across 100,000 consecutive reads.

4. **Shared-memory attach (regression test)**
   - A consumer attached through `ShmTransport` can `pop()`. Before the fix the consumer mapped the segment `PROT_READ`, and the first `pop()` (which writes `read_idx_`) crashed with SIGSEGV.

5. **Latency percentiles**
   - Nearest-rank p50/p99 on known data, with warm-up samples ignored.

---

## Performance Verification

### 1. Measured latency

`scripts/bench.sh build 20000 100`: 20,000 ticks, one every 100 µs, sent to both paths at once. The first 1,000 samples per path are dropped as warm-up, leaving 19,000 per path. Latency = receive time − send time, both read from `CLOCK_MONOTONIC`. Nearest-rank percentiles.

Host: Docker on Apple Silicon (Linux aarch64 VM, 4 CPUs allowed). 3 runs, 2026-11-07:

| Run | SHM p50 | SHM p90 | SHM p99 | TCP p50 | TCP p90 | TCP p99 |
|---|---|---|---|---|---|---|
| 1 | 291 ns | 500 ns | 18.1 µs | 21.8 µs | 36.1 µs | 166.8 µs |
| 2 | 250 ns | 334 ns | 3.4 µs | 20.4 µs | 28.8 µs | 77.7 µs |
| 3 | 125 ns | 375 ns | 8.4 µs | 20.7 µs | 29.4 µs | 105.8 µs |

- **Median: ~0.25 µs over shared memory vs ~20 µs over TCP + JSON, about 80× apart.** TCP pays for JSON encode/decode, two syscalls, socket-buffer copies and a scheduler wake-up of the blocked reader.
- **The p99 tails come from the VM**, not the queue: a vCPU gets descheduled and every tick queued behind it waits. Expect tighter tails on bare metal with isolated cores (`isolcpus`, `nohz_full`).
- **Clock resolution:** the ARM virtual counter here runs at 24 MHz, so timestamps move in 41.7 ns steps (the 41 ns minimum is one tick). Sub-microsecond numbers are quantised to that.
- These are numbers for comparing the two designs on one machine, not production capacity.

### Bugs found by measuring

The first version printed latencies, but they were not real. Fixes are in this repo, with tests where a test can catch it:

1. **The consumer crashed on its first message.** It mapped the segment `PROT_READ`, but `pop()` writes `read_idx_`, which lives in the segment, so the write raised SIGSEGV. The consumer now maps read-write. Regression test: `ShmTransport: attached consumer can pop`.
2. **Latency was only accurate to ±1 ms.** Ticks were stamped with the cached clock, which a background thread refreshes every 1 ms. Ticks are now stamped with `precise_now_ns()`.
3. **The clock was not monotonic.** It used `high_resolution_clock`, which is `system_clock` on libstdc++ and can jump when NTP corrects the wall clock. It now uses `steady_clock`.
4. **The consumer slept when idle**, adding the OS wake-up delay to any tick that arrived during the sleep. It now busy-polls with a `pause`/`yield` hint.
5. **The project did not build on a clean machine.** `shm_subscriber` was not linked to `nlohmann_json`, and the tests used Catch2 v2's `Approx` against Catch2 v3.
6. **The publisher hung if the consumer stopped**, because the loop counted only successful pushes. It now counts attempts.

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
- [x] Background monotonic clock cache (coarse) + precise `CLOCK_MONOTONIC` stamps for latency
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
