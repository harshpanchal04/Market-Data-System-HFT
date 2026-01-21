#pragma once

/**
 * @file spsc_queue.hpp
 * @brief Wait-free Single-Producer / Single-Consumer circular queue.
 *
 * An SPSC ring buffer is the canonical lock-free data structure for
 * high-frequency intra-process communication. It requires exactly one
 * producing thread and one consuming thread, which eliminates the need for
 * compare-and-swap loops and keeps the algorithm genuinely wait-free.
 *
 * Key design choices:
 *  - Power-of-two capacity allows index masking with bitwise AND instead of
 *    the more expensive modulo (%) operation.
 *  - Write index and read index are placed on separate 64-byte cache lines to
 *    prevent false sharing: the producer updates write_idx on its own cache
 *    line; the consumer updates read_idx on its own line.
 *  - Release/acquire memory ordering ensures that all writes to buffer[slot]
 *    are visible to the consumer before the updated write_idx is observed.
 */

#include <atomic>
#include <cstddef>
#include "tick_data.hpp"

namespace mds {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Default queue depth. Must be a power of two. 1024 entries × 64 bytes =
/// 64 KiB, comfortably fitting in a typical L2 cache.
constexpr std::size_t QUEUE_CAPACITY = 1024;

static_assert((QUEUE_CAPACITY & (QUEUE_CAPACITY - 1)) == 0,
    "QUEUE_CAPACITY must be a power of two");
static_assert(QUEUE_CAPACITY >= 64,
    "QUEUE_CAPACITY must be at least 64");
static_assert(QUEUE_CAPACITY <= 65536,
    "QUEUE_CAPACITY above 64 K results in excessive memory use");

// ---------------------------------------------------------------------------
// SpscQueue
// ---------------------------------------------------------------------------

/**
 * @class SpscQueue
 * @brief Wait-free ring buffer for TickData, SPSC variant.
 *
 * Producer API  — call from exactly one thread:
 *   bool push(const TickData&)  — returns false if full, true on success.
 *
 * Consumer API  — call from exactly one thread:
 *   bool pop(TickData&)         — returns false if empty, true on success.
 *
 * Diagnostic API (safe from any thread, best-effort):
 *   size_t size()     — approximate number of items currently queued.
 *   bool   empty()    — true when the queue contains no items.
 *   bool   full()     — true when no more items can be pushed.
 *   size_t capacity() — maximum number of items the queue can hold.
 */
class alignas(64) SpscQueue {
public:
    SpscQueue() = default;

    // Non-copyable, non-movable (atomics are not copyable)
    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&)                 = delete;
    SpscQueue& operator=(SpscQueue&&)      = delete;

    // ------------------------------------------------------------------
    // Producer interface
    // ------------------------------------------------------------------

    /**
     * Attempt to enqueue one TickData item.
     *
     * @param data Item to enqueue.
     * @return true on success; false if the queue is full (caller should
     *         back off or drop the tick).
     *
     * Memory ordering: the store to write_idx uses release semantics so that
     * all preceding writes to storage_[slot] are visible to the consumer
     * before it observes the updated index.
     */
    [[nodiscard]] bool push(const TickData& data) noexcept {
        const std::size_t w    = write_idx_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & mask_;
        if (next == read_idx_.load(std::memory_order_acquire)) {
            return false; // queue full
        }
        storage_[w] = data;
        write_idx_.store(next, std::memory_order_release);
        return true;
    }

    // ------------------------------------------------------------------
    // Consumer interface
    // ------------------------------------------------------------------

    /**
     * Attempt to dequeue one TickData item.
     *
     * @param[out] data Receives the dequeued item on success.
     * @return true on success; false if the queue is empty.
     *
     * Memory ordering: read_idx_ is loaded with relaxed ordering (only the
     * consumer touches it), while write_idx_ is acquired so that the
     * producer's data writes are visible.
     */
    [[nodiscard]] bool pop(TickData& data) noexcept {
        const std::size_t r = read_idx_.load(std::memory_order_relaxed);
        if (r == write_idx_.load(std::memory_order_acquire)) {
            return false; // queue empty
        }
        data = storage_[r];
        read_idx_.store((r + 1) & mask_, std::memory_order_release);
        return true;
    }

    // ------------------------------------------------------------------
    // Diagnostic API (approximate, not linearisable)
    // ------------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept {
        return read_idx_.load(std::memory_order_relaxed) ==
               write_idx_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        const std::size_t w    = write_idx_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & mask_;
        return next == read_idx_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t w = write_idx_.load(std::memory_order_acquire);
        const std::size_t r = read_idx_.load(std::memory_order_relaxed);
        return (w - r) & mask_;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return QUEUE_CAPACITY - 1; // one slot reserved for full/empty distinction
    }

    /// Raw buffer pointer, exposed for prefetch hints in the publisher.
    [[nodiscard]] const TickData* data() const noexcept {
        return storage_;
    }

    /// Current write index (for prefetch arithmetic).
    [[nodiscard]] std::size_t write_pos() const noexcept {
        return write_idx_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t mask_ = QUEUE_CAPACITY - 1;

    /// Producer-owned index, on its own cache line.
    alignas(64) std::atomic<std::size_t> write_idx_{0};

    /// Consumer-owned index, on its own cache line.
    alignas(64) std::atomic<std::size_t> read_idx_{0};

    /// The tick storage array.
    TickData storage_[QUEUE_CAPACITY];
};

// ---------------------------------------------------------------------------
// Static assertions
// ---------------------------------------------------------------------------

static_assert(alignof(SpscQueue) == 64,
    "SpscQueue must be 64-byte aligned to prevent false sharing");

} // namespace mds
