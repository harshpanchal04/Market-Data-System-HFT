#pragma once

/**
 * @file monotonic_clock.hpp
 * @brief Low-overhead monotonic timestamp source for the HFT data pipeline.
 *
 * Calling std::chrono::high_resolution_clock::now() or clock_gettime() on
 * every tick incurs a vDSO or syscall round-trip that can add hundreds of
 * nanoseconds of jitter to the hot path. MonotonicClock solves this by
 * running a background refresher thread that snapshots the system clock at a
 * configurable interval and stores the value in a lock-free atomic variable.
 * The hot path then reads a single atomic integer — no syscall, no lock.
 *
 * Trade-off: timestamps are accurate to within ~REFRESH_INTERVAL_MS
 * milliseconds. For market-data timestamping at microsecond granularity,
 * 1 ms error is acceptable; for nanosecond-accurate order entry, use
 * direct clock_gettime(CLOCK_REALTIME_COARSE) instead.
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>

namespace mds {

/**
 * @class MonotonicClock
 * @brief Thread-safe cached high-resolution clock with configurable refresh rate.
 *
 * Usage:
 * @code
 *   mds::MonotonicClock clk;
 *   int64_t ts = clk.now_ns();  // sub-nanosecond overhead
 * @endcode
 *
 * Thread safety: now_ns() is safe to call from any number of threads
 * concurrently. The internal update loop runs on a private background thread
 * that is started at construction and joined at destruction.
 *
 * The class is non-copyable and non-movable because the background thread
 * captures `this` by pointer.
 */
class MonotonicClock {
public:
    /// Interval between background clock refreshes (milliseconds).
    static constexpr int REFRESH_INTERVAL_MS = 1;

    // ------------------------------------------------------------------
    // Construction / Destruction
    // ------------------------------------------------------------------

    MonotonicClock() {
        // Seed the cache with the current time before starting the thread,
        // so that the very first call to now_ns() returns a valid value.
        cache_ns_.store(sample_clock(), std::memory_order_relaxed);

        active_.store(true, std::memory_order_relaxed);
        refresher_ = std::thread(&MonotonicClock::refresh_loop, this);
    }

    ~MonotonicClock() {
        active_.store(false, std::memory_order_relaxed);
        if (refresher_.joinable()) {
            refresher_.join();
        }
    }

    // Non-copyable, non-movable
    MonotonicClock(const MonotonicClock&)            = delete;
    MonotonicClock& operator=(const MonotonicClock&) = delete;
    MonotonicClock(MonotonicClock&&)                 = delete;
    MonotonicClock& operator=(MonotonicClock&&)      = delete;

    // ------------------------------------------------------------------
    // Public interface
    // ------------------------------------------------------------------

    /**
     * Return the cached CLOCK_MONOTONIC timestamp in nanoseconds.
     * Accuracy is the refresh interval (1 ms): fine for coarse timestamps,
     * NOT for per-message latency. Use mds::precise_now_ns() for that.
     * This is the hot-path function — it performs a single atomic load
     * with relaxed ordering and no system calls.
     *
     * @return Nanosecond timestamp (may be up to REFRESH_INTERVAL_MS stale).
     */
    [[nodiscard]] int64_t now_ns() const noexcept {
        return cache_ns_.load(std::memory_order_relaxed);
    }

    /**
     * Query whether the background refresher is currently running.
     * Useful for diagnostics and testing.
     */
    [[nodiscard]] bool is_active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

private:
    // ------------------------------------------------------------------
    // Implementation
    // ------------------------------------------------------------------

    /// Read CLOCK_MONOTONIC (steady_clock) and return nanoseconds.
    /// high_resolution_clock is an alias of system_clock on libstdc++, which
    /// can jump when NTP adjusts the wall clock, so it is not used here.
    static int64_t sample_clock() noexcept {
        using Clock = std::chrono::steady_clock;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   Clock::now().time_since_epoch())
            .count();
    }

    /// Background thread entry point: refreshes cache_ns_ every
    /// REFRESH_INTERVAL_MS milliseconds until active_ is cleared.
    void refresh_loop() {
        while (active_.load(std::memory_order_relaxed)) {
            cache_ns_.store(sample_clock(), std::memory_order_relaxed);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(REFRESH_INTERVAL_MS));
        }
    }

    // ------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------

    /// Cached nanosecond timestamp (updated by background thread).
    alignas(64) std::atomic<int64_t> cache_ns_{0};

    /// Controls the background thread lifecycle.
    std::atomic<bool> active_{false};

    /// The background refresher thread.
    std::thread refresher_;
};

} // namespace mds
