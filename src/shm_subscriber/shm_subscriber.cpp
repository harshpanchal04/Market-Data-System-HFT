/**
 * @file shm_subscriber.cpp
 * @brief Shared-Memory Subscriber — Process B
 *
 * Attaches to the POSIX shared-memory segment created by the publisher and
 * busy-polls the SpscQueue for new ticks. This is the low-latency path: no
 * kernel boundary is crossed per message and no data is copied between
 * address spaces. Both processes see the same physical pages.
 *
 * Usage:  shm_subscriber [count=1000] [warmup=0]
 *   count   stop after this many messages
 *   warmup  ignore this many initial samples (ticks that queued up before
 *           this process attached would otherwise inflate the tail)
 *
 * Mapping
 * -------
 * The consumer maps the segment read-write. pop() advances read_idx_, which
 * lives in the shared segment, so a read-only mapping would fault on the
 * first successful pop.
 *
 * Latency measurement
 * -------------------
 * The publisher stamps each tick with CLOCK_MONOTONIC (precise_now_ns) right
 * before push(). The subscriber reads the same clock right after pop(). The
 * difference is the queue traversal time plus cache-line transfer between
 * cores plus any scheduler interference.
 *
 * Spin-wait policy
 * ----------------
 * The consumer never sleeps: it spins on pop() with a cpu_relax() hint.
 * Sleeping after N empty polls (the original design) adds the OS wake-up
 * delay, typically 50+ microseconds, to every message that arrives while
 * the consumer is asleep.
 */

#include "core/tick_data.hpp"
#include "core/spsc_queue.hpp"
#include "transport/shm_transport.hpp"
#include "util/cpu_affinity.hpp"
#include "util/latency_recorder.hpp"

#include <fmt/core.h>

#include <cstddef>
#include <cstdint>

int main(int argc, char** argv) {
    const auto count  = static_cast<std::size_t>(mds::arg_or(argc, argv, 1, 1000));
    const auto warmup = static_cast<std::size_t>(mds::arg_or(argc, argv, 2, 0));

    fmt::print("=============================================\n");
    fmt::print("   Shared-Memory Subscriber  (Process B)\n");
    fmt::print("=============================================\n\n");

    try {
        // -------------------------------------------------------------------
        // 1. Pin away from the publisher (core 0) when there is a spare core
        // -------------------------------------------------------------------
        if (mds::CpuPin::logical_cpu_count() > 1 && mds::CpuPin::bind(1)) {
            fmt::print("[SYS] Subscriber pinned to CPU core 1\n");
        }

        // -------------------------------------------------------------------
        // 2. Attach to shared memory (read-write: pop() writes read_idx_)
        // -------------------------------------------------------------------
        fmt::print("[SHM] Attaching to segment 'mds_ticks'...\n");
        mds::ShmTransport shm("mds_ticks", sizeof(mds::SpscQueue), /*create=*/false);
        if (!shm.valid()) {
            fmt::print("[SHM] ERROR: Could not attach. Is the publisher running?\n");
            return 1;
        }
        fmt::print("[SHM] Attached ({} bytes)\n", shm.size());

        mds::SpscQueue* queue = shm.ptr<mds::SpscQueue>();

        // -------------------------------------------------------------------
        // 3. Busy-poll loop
        // -------------------------------------------------------------------
        fmt::print("[SUB] Polling for {} messages (warm-up {}).\n\n", count, warmup);

        mds::LatencyRecorder rec(count, warmup);
        std::size_t          consumed = 0;

        while (consumed < count) {
            mds::TickData tick;
            if (!queue->pop(tick)) {
                mds::cpu_relax();
                continue;
            }
            const int64_t latency_ns = mds::precise_now_ns() - tick.timestamp_ns;
            rec.record(latency_ns);
            ++consumed;

            // Printing is slow; keep it off the hot path except for a sample.
            if (consumed <= 5 || consumed % 5000 == 0) {
                fmt::print("[MSG #{:6}] {:12s}  bid={:8.2f}  ask={:8.2f}  latency={:.3f}us\n",
                           consumed, tick.symbol, tick.bid_price, tick.ask_price,
                           latency_ns / 1e3);
            }
        }

        // -------------------------------------------------------------------
        // 4. Final report
        // -------------------------------------------------------------------
        fmt::print("\n=== Final Shared-Memory Latency Report ===\n");
        rec.report("shm");
        fmt::print("==========================================\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] {}\n", e.what());
        return 1;
    }

    return 0;
}
