/**
 * @file test_tick_data.cpp
 * @brief Property-based and unit tests for core data structures.
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include "core/tick_data.hpp"
#include "core/spsc_queue.hpp"
#include "core/monotonic_clock.hpp"
#include "util/cpu_affinity.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// TickData
// ===========================================================================

TEST_CASE("TickData: struct invariants", "[tick_data]") {
    SECTION("size and alignment") {
        STATIC_REQUIRE(sizeof(mds::TickData) == 64);
        STATIC_REQUIRE(alignof(mds::TickData) == 64);
    }

    SECTION("default construction zeros all fields") {
        mds::TickData td;
        REQUIRE(td.bid_price    == 0.0);
        REQUIRE(td.ask_price    == 0.0);
        REQUIRE(td.timestamp_ns == 0);
        REQUIRE(td.symbol[0]    == '\0');
    }

    SECTION("parameterised construction preserves values") {
        mds::TickData td("RELIANCE", 2850.25, 2850.50, 1'737'350'400'000'000'000LL);
        REQUIRE(std::string(td.symbol) == "RELIANCE");
        REQUIRE(td.bid_price    == Approx(2850.25));
        REQUIRE(td.ask_price    == Approx(2850.50));
        REQUIRE(td.timestamp_ns == 1'737'350'400'000'000'000LL);
    }

    SECTION("symbol is null-terminated and never overflows 16 bytes") {
        mds::TickData td("AVERYLONGSYMBOLNAME_OVERFLOW", 100.0, 101.0, 0);
        REQUIRE(td.symbol[mds::SYMBOL_MAX_LEN - 1] == '\0');
    }
}

TEST_CASE("TickData: JSON round-trip", "[tick_data]") {
    SECTION("to_json / from_json are inverse operations") {
        mds::TickData original("INFY", 1780.30, 1780.55, 9'999'999'999LL);
        std::string json = original.to_json();

        mds::TickData decoded;
        REQUIRE(mds::TickData::from_json(json, decoded));
        REQUIRE(std::string(decoded.symbol) == "INFY");
        REQUIRE(decoded.bid_price    == Approx(original.bid_price));
        REQUIRE(decoded.ask_price    == Approx(original.ask_price));
        REQUIRE(decoded.timestamp_ns == original.timestamp_ns);
    }

    SECTION("from_json returns false on malformed input") {
        mds::TickData out;
        REQUIRE_FALSE(mds::TickData::from_json("{not valid json", out));
        REQUIRE_FALSE(mds::TickData::from_json("{}", out));
        REQUIRE_FALSE(mds::TickData::from_json("", out));
    }
}

// ===========================================================================
// SpscQueue
// ===========================================================================

TEST_CASE("SpscQueue: initial state", "[spsc_queue]") {
    mds::SpscQueue q;
    REQUIRE(q.empty());
    REQUIRE_FALSE(q.full());
    REQUIRE(q.size() == 0);
    REQUIRE(q.capacity() == mds::QUEUE_CAPACITY - 1);
}

TEST_CASE("SpscQueue: single push/pop", "[spsc_queue]") {
    mds::SpscQueue q;
    mds::TickData  in("SBI", 450.0, 450.5, 42LL);

    REQUIRE(q.push(in));
    REQUIRE_FALSE(q.empty());

    mds::TickData out;
    REQUIRE(q.pop(out));
    REQUIRE(std::string(out.symbol) == "SBI");
    REQUIRE(out.bid_price    == Approx(in.bid_price));
    REQUIRE(out.timestamp_ns == in.timestamp_ns);
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue: FIFO ordering", "[spsc_queue]") {
    mds::SpscQueue q;
    constexpr int N = 200;

    for (int i = 0; i < N; ++i) {
        mds::TickData td;
        std::strncpy(td.symbol, "TEST", mds::SYMBOL_MAX_LEN - 1);
        td.timestamp_ns = static_cast<int64_t>(i);
        REQUIRE(q.push(td));
    }

    for (int i = 0; i < N; ++i) {
        mds::TickData td;
        REQUIRE(q.pop(td));
        REQUIRE(td.timestamp_ns == static_cast<int64_t>(i));
    }
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue: capacity limit", "[spsc_queue]") {
    mds::SpscQueue q;
    mds::TickData  dummy("X", 1.0, 1.1, 0);

    std::size_t pushed = 0;
    while (q.push(dummy)) ++pushed;

    REQUIRE(q.full());
    REQUIRE(pushed == q.capacity());

    mds::TickData out;
    REQUIRE(q.pop(out));
    REQUIRE(q.push(dummy));
}

TEST_CASE("SpscQueue: wrap-around correctness", "[spsc_queue]") {
    mds::SpscQueue q;
    mds::TickData  dummy("WRAP", 0.0, 0.0, 0);
    mds::TickData  out;

    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < q.capacity(); ++i) {
            dummy.timestamp_ns = static_cast<int64_t>(i);
            REQUIRE(q.push(dummy));
        }
        REQUIRE(q.full());
        for (std::size_t i = 0; i < q.capacity(); ++i) {
            REQUIRE(q.pop(out));
            REQUIRE(out.timestamp_ns == static_cast<int64_t>(i));
        }
        REQUIRE(q.empty());
    }
}

TEST_CASE("SpscQueue: producer-consumer thread stress test", "[spsc_queue]") {
    mds::SpscQueue q;
    constexpr int  N = 5000;

    std::vector<int64_t> received;
    received.reserve(N);

    std::thread producer([&q] {
        mds::TickData td;
        for (int i = 0; i < N; ++i) {
            td.timestamp_ns = static_cast<int64_t>(i);
            while (!q.push(td)) std::this_thread::yield();
        }
    });

    std::thread consumer([&q, &received] {
        mds::TickData td;
        while (static_cast<int>(received.size()) < N) {
            if (q.pop(td)) received.push_back(td.timestamp_ns);
            else std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(static_cast<int>(received.size()) == N);
    for (int i = 0; i < N; ++i)
        REQUIRE(received[static_cast<std::size_t>(i)] == static_cast<int64_t>(i));
}

// ===========================================================================
// MonotonicClock
// ===========================================================================

TEST_CASE("MonotonicClock: basic properties", "[clock]") {
    mds::MonotonicClock clk;

    SECTION("returns positive timestamp") {
        REQUIRE(clk.now_ns() > 0);
    }

    SECTION("is active after construction") {
        REQUIRE(clk.is_active());
    }

    SECTION("timestamps are non-decreasing") {
        int64_t t1 = clk.now_ns();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(mds::MonotonicClock::REFRESH_INTERVAL_MS + 5));
        int64_t t2 = clk.now_ns();
        REQUIRE(t2 >= t1);
    }
}

// ===========================================================================
// Utilities
// ===========================================================================

TEST_CASE("CpuPin: logical CPU count is positive", "[util]") {
    REQUIRE(mds::CpuPin::logical_cpu_count() > 0);
}

TEST_CASE("MemAlign: is_aligned correctness", "[util]") {
    alignas(64) char buf[128];
    REQUIRE(mds::MemAlign::is_aligned(buf, 64));
    REQUIRE_FALSE(mds::MemAlign::is_aligned(buf + 1, 64));
}

TEST_CASE("MemAlign: type alignment for core types", "[util]") {
    REQUIRE(mds::MemAlign::type_is_aligned<mds::TickData>(64));
    REQUIRE(mds::MemAlign::type_is_aligned<mds::SpscQueue>(64));
}
