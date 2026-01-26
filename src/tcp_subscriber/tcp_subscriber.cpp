/**
 * @file tcp_subscriber.cpp
 * @brief TCP Subscriber — Process C
 *
 * Connects to the publisher's TCP server (port 8800) and receives newline-
 * delimited JSON tick messages. Each message is deserialized into a TickData
 * struct and its embedded nanosecond timestamp is compared to the local
 * high-resolution clock to produce an end-to-end latency sample.
 *
 * The TCP path is inherently higher-latency than shared memory because:
 *  - Data is copied through the kernel socket buffer.
 *  - Nagle's algorithm batches small writes (mitigated by TCP_NODELAY on the
 *    publisher side).
 *  - The OS scheduler introduces jitter on send and receive.
 *
 * Despite the latency overhead, the TCP path is the canonical channel for
 * distributing market data to remote consumers, and is the channel used by
 * exchange gateways and market data vendors in production.
 */

#include "core/tick_data.hpp"

#include <boost/asio.hpp>
#include <fmt/core.h>
#include <fmt/chrono.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace asio = boost::asio;
using  tcp     = asio::ip::tcp;

// ===========================================================================
// LatencyStats (same as shm_subscriber — kept local to avoid shared header)
// ===========================================================================

struct LatencyStats {
    std::size_t count    = 0;
    int64_t     total_ns = 0;
    int64_t     min_ns   = INT64_MAX;
    int64_t     max_ns   = 0;

    void update(int64_t lat) noexcept {
        ++count;
        total_ns += lat;
        min_ns    = std::min(min_ns, lat);
        max_ns    = std::max(max_ns, lat);
    }

    void print() const {
        if (count == 0) { fmt::print("  (no data)\n"); return; }
        fmt::print("  samples={:5}  avg={:.3f}µs  min={:.3f}µs  max={:.3f}µs\n",
                   count,
                   (static_cast<double>(total_ns) / count) / 1e3,
                   min_ns / 1e3,
                   max_ns / 1e3);
    }
};

// ===========================================================================
// main
// ===========================================================================

int main() {
    fmt::print("=============================================\n");
    fmt::print("   TCP Subscriber  (Process C)\n");
    fmt::print("=============================================\n\n");

    try {
        // -------------------------------------------------------------------
        // 1. Connect to publisher
        // -------------------------------------------------------------------
        fmt::print("[NET] Connecting to publisher at 127.0.0.1:8800...\n");
        asio::io_context io;
        tcp::socket      sock(io);
        tcp::resolver    resolver(io);

        asio::connect(sock, resolver.resolve("127.0.0.1", "8800"));
        fmt::print("[NET] Connected.\n\n");

        // -------------------------------------------------------------------
        // 2. Receive-and-parse loop
        // -------------------------------------------------------------------
        fmt::print("[SUB] Waiting for ticks. Press Ctrl+C to stop.\n\n");

        asio::streambuf rx_buf;
        std::size_t     received    = 0;
        std::size_t     parse_errs  = 0;
        LatencyStats    stats;

        while (true) {
            try {
                // Block until a complete newline-delimited message arrives.
                asio::read_until(sock, rx_buf, '\n');
            } catch (const boost::system::system_error& e) {
                if (e.code() == asio::error::eof ||
                    e.code() == asio::error::connection_reset) {
                    fmt::print("[NET] Publisher disconnected.\n");
                } else {
                    fmt::print("[NET] Network error: {} ({})\n",
                               e.what(), e.code().value());
                }
                break;
            }

            // Extract the next newline-terminated line from the buffer.
            std::istream stream(&rx_buf);
            std::string  line;
            std::getline(stream, line);
            if (line.empty()) continue;

            ++received;

            // Capture receive timestamp immediately after dequeue.
            const int64_t recv_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch())
                    .count();

            mds::TickData tick;
            if (!mds::TickData::from_json(line, tick)) {
                ++parse_errs;
                fmt::print("[WARN] Parse error on message #{}: {}\n",
                           received, line.substr(0, 80));
                continue;
            }

            const int64_t latency_ns = recv_ns - tick.timestamp_ns;
            stats.update(latency_ns);

            fmt::print(
                "[MSG #{:4}] {:12s}  BID={:8.2f}  ASK={:8.2f}  lat={:.2f}µs\n",
                received,
                tick.symbol,
                tick.bid_price,
                tick.ask_price,
                latency_ns / 1e3);

            if (received % 10 == 0) {
                fmt::print("--- TCP latency after {} messages ---\n", received);
                stats.print();
                fmt::print("\n");
            }

            if (received >= 50) {
                fmt::print("\n[SUB] Received {} ticks — stopping.\n\n", received);
                break;
            }
        }

        // -------------------------------------------------------------------
        // 3. Final report
        // -------------------------------------------------------------------
        fmt::print("=== Final TCP Latency Report ===\n");
        stats.print();
        fmt::print("Parse errors: {}\n", parse_errs);
        fmt::print("================================\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] {}\n", e.what());
        return 1;
    }

    return 0;
}
