#pragma once

/**
 * @file cpu_affinity.hpp
 * @brief CPU core affinity and memory alignment utilities.
 *
 * Provides thin, platform-agnostic wrappers around Linux pthread affinity
 * APIs and macOS thread-affinity hints, together with compile-time and
 * runtime memory-alignment helpers used across the hot path.
 */

#include <thread>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#ifdef __linux__
#  include <sched.h>
#  include <pthread.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <mach/thread_policy.h>
#  include <mach/thread_act.h>
#  include <mach/mach.h>
#  include <pthread.h>
#  include <sys/sysctl.h>
#endif

namespace mds {

// ---------------------------------------------------------------------------
// CpuPin — CPU affinity helpers
// ---------------------------------------------------------------------------

/**
 * @class CpuPin
 * @brief Static utility methods for pinning threads to specific CPU cores.
 *
 * Pinning a latency-critical thread to a dedicated, isolated core eliminates
 * context-switch overhead and dramatically reduces cache pollution. On Linux
 * this maps directly to pthread_setaffinity_np(3). On macOS, true CPU pinning
 * is not supported by the kernel; thread_policy_set with an affinity tag is
 * a best-effort hint that influences the scheduler but does not guarantee
 * exclusive use of a core.
 */
struct CpuPin {
    /**
     * Bind the calling thread to CPU core @p core_id.
     * @return true on success, false if the platform does not support it or
     *         if core_id is out of range.
     */
    static bool bind(int core_id) noexcept {
        try {
#ifdef __linux__
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(core_id, &mask);
            return pthread_setaffinity_np(
                       pthread_self(), sizeof(cpu_set_t), &mask) == 0;

#elif defined(__APPLE__)
            thread_affinity_policy_data_t pol = {core_id};
            return thread_policy_set(
                       pthread_mach_thread_np(pthread_self()),
                       THREAD_AFFINITY_POLICY,
                       reinterpret_cast<thread_policy_t>(&pol),
                       THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
            (void)core_id;
            return false; // unsupported platform
#endif
        } catch (...) {
            return false;
        }
    }

    /**
     * Return the number of logical CPUs available to the process.
     * Falls back to std::thread::hardware_concurrency() on unknown platforms.
     */
    static int logical_cpu_count() noexcept {
        try {
#ifdef __linux__
            long n = ::sysconf(_SC_NPROCESSORS_ONLN);
            return n > 0 ? static_cast<int>(n) : 1;
#elif defined(__APPLE__)
            int n = 0;
            std::size_t len = sizeof(n);
            return (::sysctlbyname("hw.logicalcpu", &n, &len, nullptr, 0) == 0 && n > 0)
                       ? n : 1;
#else
            return static_cast<int>(std::thread::hardware_concurrency());
#endif
        } catch (...) {
            return 1;
        }
    }

    /**
     * Return the CPU on which the calling thread is currently executing.
     * Returns -1 if the platform does not expose this information.
     */
    static int current_cpu() noexcept {
#ifdef __linux__
        return ::sched_getcpu();
#else
        return -1;
#endif
    }
};

// ---------------------------------------------------------------------------
// MemAlign — alignment / prefetch helpers
// ---------------------------------------------------------------------------

/**
 * @class MemAlign
 * @brief Compile-time and runtime memory alignment utilities.
 */
struct MemAlign {
    /**
     * Return true when @p ptr is aligned to at least @p alignment bytes.
     * @p alignment must be a power of two.
     */
    template <typename T>
    static bool is_aligned(const T* ptr, std::size_t alignment) noexcept {
        if (!ptr || alignment == 0 || (alignment & (alignment - 1)) != 0)
            return false;
        return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1)) == 0;
    }

    /**
     * Return true when the natural alignment of type T is at least
     * @p alignment bytes (compile-time equivalent of is_aligned).
     */
    template <typename T>
    static constexpr bool type_is_aligned(std::size_t alignment) noexcept {
        return alignof(T) >= alignment;
    }

    /**
     * Return the L1 data-cache line size reported by the OS.
     * Defaults to 64 bytes (standard x86-64 value) if the OS does not
     * expose the information.
     */
    static std::size_t cache_line_bytes() noexcept {
        try {
#ifdef __linux__
            long v = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
            if (v > 0) return static_cast<std::size_t>(v);
#elif defined(__APPLE__)
            std::size_t v = 0;
            std::size_t len = sizeof(v);
            if (::sysctlbyname("hw.cachelinesize", &v, &len, nullptr, 0) == 0 && v > 0)
                return v;
#endif
            return 64;
        } catch (...) {
            return 64;
        }
    }

    /// Hint to the CPU to prefetch the cache line at @p addr for a subsequent read.
    static void prefetch_read(const void* addr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(addr, 0, 3);
#elif defined(_MSC_VER)
        _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T0);
#else
        (void)addr;
#endif
    }

    /// Hint to the CPU to prefetch the cache line at @p addr for a subsequent write.
    static void prefetch_write(const void* addr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(addr, 1, 3);
#elif defined(_MSC_VER)
        _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T0);
#else
        (void)addr;
#endif
    }
};

// ---------------------------------------------------------------------------
// MemoryPool — fixed-size object pool for hot-path allocation avoidance
// ---------------------------------------------------------------------------

/**
 * @class MemoryPool
 * @brief Pre-allocated pool of @p N objects of type @p T.
 *
 * Avoids dynamic allocation in the hot path. Allocation is O(N) in the
 * worst case due to the linear scan; for small pools this is acceptable and
 * preferable to heap allocation.
 *
 * Not thread-safe. Use from a single thread only.
 *
 * @tparam T        Object type to pool.
 * @tparam N        Pool size (maximum live objects).
 */
template <typename T, std::size_t N>
class alignas(64) MemoryPool {
public:
    MemoryPool() {
        for (std::size_t i = 0; i < N; ++i) used_[i] = false;
    }

    /// Acquire an unused slot, or return nullptr if the pool is exhausted.
    T* acquire() noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            std::size_t idx = (cursor_ + i) % N;
            if (!used_[idx]) {
                used_[idx] = true;
                cursor_    = (idx + 1) % N;
                return &pool_[idx];
            }
        }
        return nullptr;
    }

    /// Release a previously acquired slot back to the pool.
    void release(T* p) noexcept {
        if (p >= pool_ && p < pool_ + N) {
            used_[static_cast<std::size_t>(p - pool_)] = false;
        }
    }

    [[nodiscard]] std::size_t free_slots() const noexcept {
        std::size_t cnt = 0;
        for (std::size_t i = 0; i < N; ++i) {
            if (!used_[i]) ++cnt;
        }
        return cnt;
    }

    static constexpr std::size_t capacity() noexcept { return N; }

private:
    alignas(64) T    pool_[N];
    alignas(64) bool used_[N];
    std::size_t      cursor_ = 0;
};

} // namespace mds
