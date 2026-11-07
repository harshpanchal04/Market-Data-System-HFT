/**
 * @file tcp_subscriber.cpp
 * @brief TCP Subscriber — Process C
 *
 * Connects to the publisher's TCP server (port 8800) and receives newline-
 * delimited JSON tick messages. Each message is parsed back into a TickData
 * and its embedded CLOCK_MONOTONIC timestamp is compared with the same clock
 * on arrival to produce an end-to-end latency sample.
 *
 * Usage:  tcp_subscriber [count=50] [warmup=0]
 *
 * The TCP path is slower than shared memory because:
 *  - Each tick is serialised to JSON and parsed back.
 *  - Data is copied into and out of kernel socket buffers (2 syscalls).
 *  - The receiver blocks in the kernel and must be woken by the scheduler.
 * Nagle's algorithm is disabled on the publisher socket (TCP_NODELAY).
 *
 * Despite the overhead, TCP is the canonical channel for distributing market
 * data to remote consumers.
 */

#include "core/tick_data.hpp"
#include "util/latency_recorder.hpp"

#include <boost/asio.hpp>
#include <fmt/core.h>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>

namespace asio = boost::asio;
using  tcp     = asio::ip::tcp;

int main(int argc, char** argv) {
    const auto count  = static_cast<std::size_t>(mds::arg_or(argc, argv, 1, 50));
    const auto warmup = static_cast<std::size_t>(mds::arg_or(argc, argv, 2, 0));

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
        fmt::print("[SUB] Waiting for {} ticks (warm-up {}).\n\n", count, warmup);

        asio::streambuf      rx_buf;
        std::size_t          received   = 0;
        std::size_t          parse_errs = 0;
        mds::LatencyRecorder rec(count, warmup);

        while (received < count) {
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

            // One line per iteration. If several lines arrived together,
            // the next read_until returns at once without a syscall.
            std::istream stream(&rx_buf);
            std::string  line;
            std::getline(stream, line);
            if (line.empty()) continue;

            mds::TickData tick;
            if (!mds::TickData::from_json(line, tick)) {
                ++parse_errs;
                continue;
            }
            // Timestamp after parsing: the JSON decode is part of this path's cost.
            const int64_t latency_ns = mds::precise_now_ns() - tick.timestamp_ns;
            rec.record(latency_ns);
            ++received;

            if (received <= 5 || received % 5000 == 0) {
                fmt::print("[MSG #{:6}] {:12s}  BID={:8.2f}  ASK={:8.2f}  lat={:.2f}us\n",
                           received, tick.symbol, tick.bid_price, tick.ask_price,
                           latency_ns / 1e3);
            }
        }

        // -------------------------------------------------------------------
        // 3. Final report
        // -------------------------------------------------------------------
        fmt::print("\n=== Final TCP Latency Report ===\n");
        rec.report("tcp");
        fmt::print("Parse errors: {}\n", parse_errs);
        fmt::print("================================\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] {}\n", e.what());
        return 1;
    }

    return 0;
}
