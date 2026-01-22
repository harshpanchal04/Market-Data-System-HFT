/**
 * @file publisher.cpp
 * @brief Market Data Publisher — Process A
 *
 * Responsibilities
 * ----------------
 * 1. Initialize a POSIX shared-memory segment and place an SpscQueue in it.
 * 2. Start a Boost.Asio TCP server on port 8800 that broadcasts JSON ticks to
 *    all connected clients (Process C).
 * 3. Generate synthetic Level-1 market ticks in a tight loop, push each tick
 *    to the shared-memory queue (ultra-low latency path) and broadcast it as
 *    JSON over TCP (network path).
 * 4. Report throughput and drop statistics every 100 ticks.
 *
 * Design notes
 * ------------
 * - MonotonicClock is used for all tick timestamps to avoid syscall overhead.
 * - CPU affinity is pinned to core 0 so that the publisher thread does not
 *   migrate across cores during the generation loop.
 * - TCP_NODELAY is enabled on every accepted client socket to disable Nagle's
 *   algorithm, reducing TCP latency by an order of magnitude for small,
 *   frequent messages.
 * - The io_context runs in a dedicated thread so that asynchronous I/O does
 *   not block the data-generation loop.
 */

#include "core/tick_data.hpp"
#include "core/spsc_queue.hpp"
#include "core/monotonic_clock.hpp"
#include "transport/shm_transport.hpp"
#include "util/cpu_affinity.hpp"

#include <boost/asio.hpp>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using  tcp     = asio::ip::tcp;

// ===========================================================================
// TcpBroadcaster
// ===========================================================================

/**
 * @class TcpBroadcaster
 * @brief Manages TCP client connections and broadcasts JSON tick messages.
 *
 * Accepts connections asynchronously. For each connected socket, a read is
 * posted to detect disconnection without blocking the broadcast path.
 * Disconnected clients are pruned lazily on the next broadcast.
 */
class TcpBroadcaster {
public:
    TcpBroadcaster(asio::io_context& io, unsigned short port)
        : io_(io)
        , acceptor_(io, tcp::endpoint(tcp::v4(), port))
    {
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        fmt::print("[TCP] Listening on 0.0.0.0:{}\n", port);
        accept_next();
    }

    /// Send @p msg to every connected client, dropping stragglers.
    void broadcast(const std::string& msg) {
        const std::string framed = msg + '\n'; // newline-delimited framing
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = clients_.begin(); it != clients_.end(); ) {
            auto sock = *it;
            if (!sock->is_open()) {
                it = clients_.erase(it);
                continue;
            }
            asio::async_write(*sock, asio::buffer(framed),
                [sock](boost::system::error_code /*ec*/, std::size_t) {});
            ++it;
        }
    }

    [[nodiscard]] std::size_t client_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clients_.size();
    }

private:
    void accept_next() {
        auto sock = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*sock,
            [this, sock](boost::system::error_code ec) {
                if (!ec) {
                    tune_socket(*sock);
                    fmt::print("[TCP] Client connected: {}\n",
                               sock->remote_endpoint().address().to_string());
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        clients_.insert(sock);
                    }
                    watch_disconnect(sock);
                }
                accept_next();
            });
    }

    void watch_disconnect(std::shared_ptr<tcp::socket> sock) {
        auto buf = std::make_shared<std::array<char, 1>>();
        sock->async_read_some(asio::buffer(*buf),
            [this, sock, buf](boost::system::error_code ec, std::size_t) {
                if (ec) {
                    fmt::print("[TCP] Client disconnected: {}\n", ec.message());
                    std::lock_guard<std::mutex> lk(mu_);
                    clients_.erase(sock);
                } else {
                    watch_disconnect(sock);
                }
            });
    }

    static void tune_socket(tcp::socket& sock) {
        try {
            sock.set_option(tcp::no_delay(true));
            sock.set_option(asio::socket_base::send_buffer_size(64 * 1024));
            sock.set_option(asio::socket_base::receive_buffer_size(64 * 1024));
        } catch (const std::exception& e) {
            fmt::print("[TCP] Warning: socket tuning failed: {}\n", e.what());
        }
    }

    asio::io_context&                                   io_;
    tcp::acceptor                                       acceptor_;
    std::set<std::shared_ptr<tcp::socket>>              clients_;
    mutable std::mutex                                  mu_;
};

// ===========================================================================
// Instrument universe
// ===========================================================================

static const std::vector<const char*> SYMBOLS = {
    "RELIANCE", "TCS", "INFY", "HDFC", "ICICI",
    "SBI",      "ITC", "AXISBANK", "BAJFINANCE", "WIPRO",
    "ONGC",     "NTPC", "HCLTECH", "TITAN", "SUNPHARMA",
    "DRREDDY",  "MARUTI", "JSWSTEEL", "HINDALCO", "COALINDIA",
};

// ===========================================================================
// main
// ===========================================================================

int main() {
    fmt::print("=============================================\n");
    fmt::print("   Market Data Publisher  (Process A)\n");
    fmt::print("=============================================\n\n");

    // -----------------------------------------------------------------------
    // 1. System diagnostics and CPU affinity
    // -----------------------------------------------------------------------
    const int  ncpus     = mds::CpuPin::logical_cpu_count();
    const auto cline_sz  = mds::MemAlign::cache_line_bytes();
    fmt::print("[SYS] Logical CPUs: {}  |  Cache-line: {} bytes\n", ncpus, cline_sz);

    if (mds::CpuPin::bind(0)) {
        int on = mds::CpuPin::current_cpu();
        fmt::print("[SYS] Publisher pinned to CPU core 0 (currently on {})\n", on);
    } else {
        fmt::print("[SYS] Warning: CPU pinning not supported on this platform\n");
    }

    fmt::print("[SYS] TickData  64-byte aligned: {}\n",
               mds::MemAlign::type_is_aligned<mds::TickData>(64)  ? "YES" : "NO");
    fmt::print("[SYS] SpscQueue 64-byte aligned: {}\n",
               mds::MemAlign::type_is_aligned<mds::SpscQueue>(64) ? "YES" : "NO");

    try {
        // -------------------------------------------------------------------
        // 2. Shared-memory setup
        // -------------------------------------------------------------------
        fmt::print("\n[SHM] Creating shared-memory segment 'mds_ticks'...\n");
        mds::ShmTransport shm("mds_ticks", sizeof(mds::SpscQueue), /*create=*/true);
        if (!shm.valid()) {
            fmt::print("[SHM] ERROR: segment creation failed\n");
            return 1;
        }
        fmt::print("[SHM] Segment ready ({} bytes)\n", shm.size());

        // Placement-new the queue inside shared memory.
        mds::SpscQueue* queue = new (shm.ptr<void>()) mds::SpscQueue{};
        fmt::print("[SHM] SpscQueue constructed in shared memory\n");

        // -------------------------------------------------------------------
        // 3. TCP broadcaster
        // -------------------------------------------------------------------
        fmt::print("\n[TCP] Starting broadcaster...\n");
        asio::io_context io;
        TcpBroadcaster   broadcaster(io, 8800);

        std::thread io_thread([&io] { io.run(); });

        // -------------------------------------------------------------------
        // 4. Clock
        // -------------------------------------------------------------------
        mds::MonotonicClock clk;
        fmt::print("[CLK] MonotonicClock started (refresh: {} ms)\n",
                   mds::MonotonicClock::REFRESH_INTERVAL_MS);

        // -------------------------------------------------------------------
        // 5. Tick-generation loop
        // -------------------------------------------------------------------
        std::mt19937_64                     rng(std::random_device{}());
        std::uniform_real_distribution<>    price_dist(50.0, 5000.0);
        std::uniform_real_distribution<>    spread_dist(0.01, 2.0);
        std::uniform_int_distribution<>     sym_dist(
            0, static_cast<int>(SYMBOLS.size()) - 1);

        fmt::print("\n[PUB] Starting tick-generation loop. Press Ctrl+C to stop.\n\n");

        std::size_t published = 0;
        std::size_t dropped   = 0;

        while (true) {
            const char*   sym    = SYMBOLS[sym_dist(rng)];
            const double  bid    = price_dist(rng);
            const double  ask    = bid + spread_dist(rng);
            const int64_t ts     = clk.now_ns();
            mds::TickData tick(sym, bid, ask, ts);

            // Prefetch the next write slot to warm the cache line.
            {
                const std::size_t next_w = (queue->write_pos() + 1)
                                           & (mds::QUEUE_CAPACITY - 1);
                mds::MemAlign::prefetch_write(&queue->data()[next_w]);
            }

            if (queue->push(tick)) {
                ++published;
                if (broadcaster.client_count() > 0) {
                    broadcaster.broadcast(tick.to_json());
                }
            } else {
                ++dropped;
            }

            if (published % 100 == 0 && published > 0) {
                fmt::print(
                    "[PUB] published={:6}  queued={:4}/{}  dropped={}  tcp_clients={}\n",
                    published,
                    queue->size(), mds::SpscQueue::capacity(),
                    dropped,
                    broadcaster.client_count());
            }

            // ~1 000 ticks/s
            std::this_thread::sleep_for(std::chrono::microseconds(1000));

            if (published >= 1000) {
                fmt::print("\n[PUB] Reached {} published ticks — stopping.\n", published);
                break;
            }
        }

        // -------------------------------------------------------------------
        // 6. Shutdown
        // -------------------------------------------------------------------
        fmt::print("[NET] Stopping IO context...\n");
        io.stop();
        if (io_thread.joinable()) io_thread.join();

        fmt::print("[PUB] Final: published={}, dropped={}\n", published, dropped);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] {}\n", e.what());
        return 1;
    }

    return 0;
}
