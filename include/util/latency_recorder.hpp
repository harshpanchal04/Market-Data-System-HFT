#pragma once
/**
 * @file latency_recorder.hpp
 * @brief Precise timestamps, a spin-wait hint and a percentile recorder.
 *
 * Why this exists
 * ---------------
 * Latency is measured as (receive time - send time) across two processes, so
 * both sides must read the same clock with nanosecond precision.
 * `precise_now_ns()` reads std::chrono::steady_clock, which is
 * CLOCK_MONOTONIC on Linux: shared by all processes on the host, never jumps
 * backwards, and served from the vDSO (no syscall, tens of nanoseconds).
 *
 * The cached MonotonicClock is only accurate to its refresh interval
 * (1 ms), so it must not be used to timestamp messages for latency.
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <fmt/core.h>

namespace mds {

/// Nanoseconds from CLOCK_MONOTONIC (std::chrono::steady_clock).
[[nodiscard]] inline int64_t precise_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Tell the CPU we are in a spin-wait loop (saves power, frees the sibling
/// hyper-thread, avoids a memory-order pipeline flush on loop exit).
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#endif
}

/// Parse argv[i] as an integer, or return `fallback` if it is missing.
[[nodiscard]] inline long long arg_or(int argc, char** argv, int i,
                                      long long fallback) noexcept {
    return (i < argc) ? std::strtoll(argv[i], nullptr, 10) : fallback;
}

/// Summary of a latency distribution, in nanoseconds.
struct LatencySummary {
    std::size_t n    = 0;
    double      mean = 0;
    int64_t     min  = 0;
    int64_t     p50  = 0;
    int64_t     p90  = 0;
    int64_t     p99  = 0;
    int64_t     p999 = 0;
    int64_t     max  = 0;
};

/**
 * Stores every latency sample after a warm-up period, then reports
 * nearest-rank percentiles. Recording is a vector push_back into
 * pre-reserved memory, so it does not allocate on the hot path.
 */
class LatencyRecorder {
public:
    LatencyRecorder(std::size_t expected, std::size_t warmup)
        : warmup_(warmup) {
        samples_.reserve(expected);
    }

    void record(int64_t latency_ns) {
        if (seen_++ < warmup_) return;
        samples_.push_back(latency_ns);
    }

    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }

    [[nodiscard]] LatencySummary summary() const {
        LatencySummary s;
        if (samples_.empty()) return s;
        std::vector<int64_t> v(samples_);
        std::sort(v.begin(), v.end());
        auto rank = [&](double q) {
            std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(v.size()));
            if (idx >= v.size()) idx = v.size() - 1;
            return v[idx];
        };
        long double total = 0;
        for (int64_t x : v) total += x;
        s.n    = v.size();
        s.mean = static_cast<double>(total / v.size());
        s.min  = v.front();
        s.p50  = rank(0.50);
        s.p90  = rank(0.90);
        s.p99  = rank(0.99);
        s.p999 = rank(0.999);
        s.max  = v.back();
        return s;
    }

    /// Human-readable table plus one machine-readable RESULT line.
    void report(const std::string& path) const {
        const LatencySummary s = summary();
        if (s.n == 0) { fmt::print("  (no samples)\n"); return; }
        auto us = [](double ns) { return ns / 1e3; };
        fmt::print("  samples={}  mean={:.2f}us  min={:.2f}us  p50={:.2f}us  "
                   "p90={:.2f}us  p99={:.2f}us  p99.9={:.2f}us  max={:.2f}us\n",
                   s.n, us(s.mean), us(s.min), us(s.p50), us(s.p90),
                   us(s.p99), us(s.p999), us(s.max));
        fmt::print("RESULT path={} n={} mean_ns={:.0f} min_ns={} p50_ns={} "
                   "p90_ns={} p99_ns={} p999_ns={} max_ns={}\n",
                   path, s.n, s.mean, s.min, s.p50, s.p90, s.p99, s.p999, s.max);
    }

private:
    std::vector<int64_t> samples_;
    std::size_t          warmup_ = 0;
    std::size_t          seen_   = 0;
};

} // namespace mds
