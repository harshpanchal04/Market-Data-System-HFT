/**
 * @file shm_subscriber.cpp
 * @brief Shared-Memory Subscriber — Process B
 *
 * Attaches (read-only) to the POSIX shared-memory segment created by the
 * publisher and continuously polls the SpscQueue for new ticks. This is the
 * ultra-low-latency path: no kernel boundary is crossed per message, no data
 * is copied between address spaces — both processes simply see the same
 * physical memory pages.
 *
 * Latency measurement
 * -------------------
 * The publisher embeds a MonotonicClock timestamp (nanoseconds) in each tick.
 * The subscriber reads its own MonotonicClock on arrival and computes the
 * difference. Both clocks are seeded from the same high_resolution_clock
 * source, so the delta primarily reflects queue-traversal time plus any
 * OS scheduler jitter.
 *
 * Spin-wait policy
 * ----------------
 * The consumer spins on pop() with no synchronisation overhead. After 1 000
 * consecutive empty polls it yields for 1 μs to reduce CPU saturation when
 * the publisher is idle. After 10 000 consecutive empty polls the yield
 * counter is clamped to avoid integer overflow.
 */

#include "core/tick_data.hpp"
#include "core/spsc_queue.hpp"
#include "core/monotonic_clock.hpp"
#include "transport/shm_transport.hpp"

#include <fmt/core.h>
#include <fmt/chrono.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <thread>

// ===========================================================================
// LatencyStats — running statistics accumulator
// ===========================================================================

struct LatencyStats {
    std::size_t count     = 0;
    int64_t     total_ns  = 0;
    int64_t     min_ns    = INT64_MAX;
    int64_t     max_ns    = 0;

    void update(int64_t latency_ns) noexcept {
        ++count;
        total_ns += latency_ns;
        min_ns    = std::min(min_ns, latency_ns);
        max_ns    = std::max(max_ns, latency_ns);
    }

    void print_summary() const {
        if (count == 0) { fmt::print("  (no data)\n"); return; }
        double avg = (static_cast<double>(total_ns) / count) / 1e3;
        double mn  = min_ns / 1e3;
        double mx  = max_ns / 1e3;
        fmt::print("  samples={:6}  avg={:.3f}µs  min={:.3f}µs  max={:.3f}µs\n",
                   count, avg, mn, mx);
    }
};

// ===========================================================================
// main
// ===========================================================================

int main() {
    fmt::print("=============================================\n");
    fmt::print("   Shared-Memory Subscriber  (Process B)\n");
    fmt::print("=============================================\n\n");

    try {
        // -------------------------------------------------------------------
        // 1. Attach to shared memory (read-only)
        // -------------------------------------------------------------------
        fmt::print("[SHM] Attaching to segment 'mds_ticks'...\n");
        mds::ShmTransport shm("mds_ticks", sizeof(mds::SpscQueue), /*create=*/false);
        if (!shm.valid()) {
            fmt::print("[SHM] ERROR: Could not attach. Is the publisher running?\n");
            return 1;
        }
        fmt::print("[SHM] Attached ({} bytes)\n", shm.size());

        const mds::SpscQueue* queue = shm.ptr<mds::SpscQueue>();

        // -------------------------------------------------------------------
        // 2. Local clock for latency measurement
        // -------------------------------------------------------------------
        mds::MonotonicClock clk;
        fmt::print("[CLK] MonotonicClock started\n");

        // -------------------------------------------------------------------
        // 3. Polling loop
        // -------------------------------------------------------------------
        fmt::print("\n[SUB] Polling shared-memory queue. Press Ctrl+C to stop.\n\n");

        std::size_t  consumed   = 0;
        std::size_t  idle_polls = 0;
        LatencyStats stats;

        while (true) {
            mds::TickData tick;

            // const_cast: the SpscQueue consumer path mutates only read_idx_
            // which is the consumer's own cache line — safe even with the
            // shared-memory read-only mmap for the *data* pages.
            //
            // Note: in a production system the consumer would have its own
            // writable mapping of the same SHM segment for the index slot.
            if (const_cast<mds::SpscQueue*>(queue)->pop(tick)) {
                const int64_t recv_ns    = clk.now_ns();
                const int64_t latency_ns = recv_ns - tick.timestamp_ns;

                stats.update(latency_ns);
                ++consumed;
                idle_polls = 0;

                // Print first 10 ticks and then every 100th.
                if (consumed <= 10 || consumed % 100 == 0) {
                    fmt::print(
                        "[MSG #{:6}] {:12s}  bid={:8.2f}  ask={:8.2f}  "
                        "latency={:.3f}µs\n",
                        consumed,
                        tick.symbol,
                        tick.bid_price,
                        tick.ask_price,
                        latency_ns / 1e3);
                }

                // Print statistics every 100 messages.
                if (consumed % 100 == 0) {
                    fmt::print("--- Latency stats after {} messages ---\n", consumed);
                    stats.print_summary();
                    fmt::print("\n");
                }

            } else {
                ++idle_polls;
                if (idle_polls > 1000) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                    if (idle_polls > 10000) idle_polls = 1000; // clamp
                }
            }

            if (consumed >= 1000) {
                fmt::print("\n[SUB] Consumed {} ticks — stopping.\n\n", consumed);
                break;
            }
        }

        // -------------------------------------------------------------------
        // 4. Final report
        // -------------------------------------------------------------------
        fmt::print("=== Final Shared-Memory Latency Report ===\n");
        stats.print_summary();
        fmt::print("==========================================\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] {}\n", e.what());
        return 1;
    }

    return 0;
}
