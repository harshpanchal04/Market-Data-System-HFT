#pragma once

/**
 * @file tick_data.hpp
 * @brief Core market tick data structure for the HFT data distribution system.
 *
 * Defines the primary data unit flowing through all transport layers.
 * The TickData structure is designed for zero-copy, cache-friendly access
 * with explicit 64-byte cache-line alignment to eliminate false sharing.
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <nlohmann/json.hpp>

namespace mds {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Maximum number of characters (including null terminator) in a symbol name.
/// Kept at 16 for natural 64-byte alignment math and to cover all standard
/// exchange ticker symbols (e.g. "RELIANCE", "AAPL", "EURUSD").
constexpr std::size_t SYMBOL_MAX_LEN = 16;

// ---------------------------------------------------------------------------
// TickData
// ---------------------------------------------------------------------------

/**
 * @struct TickData
 * @brief Represents a single Level-1 market quote.
 *
 * Memory layout (64-byte cache-line aligned):
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *    0      16    symbol[16]      — null-terminated instrument ticker
 *   16       8    bid_price       — best bid (double)
 *   24       8    ask_price       — best ask (double)
 *   32       8    timestamp_ns    — nanoseconds since UNIX epoch (int64_t)
 *   40      24    _pad            — padding to complete 64-byte alignment
 *
 * Design rationale:
 *  - Fixed-size char array instead of std::string avoids heap allocation in
 *    the hot path and makes the struct trivially copyable.
 *  - double for prices gives ~15 significant digits, adequate for equities
 *    and FX. Production systems often use scaled int64_t; this is noted as
 *    a future optimisation.
 *  - int64_t nanosecond timestamps remain valid until year 2262.
 *  - The 64-byte alignment matches modern CPU cache line width, preventing
 *    false sharing when multiple threads access adjacent array elements.
 */
struct alignas(64) TickData {
    char     symbol[SYMBOL_MAX_LEN];  ///< Instrument ticker, null-terminated
    double   bid_price;               ///< Best bid price
    double   ask_price;               ///< Best ask price
    int64_t  timestamp_ns;            ///< Publish timestamp (ns since epoch)
    char     _pad[24];                ///< Explicit padding to 64 bytes

    // ------------------------------------------------------------------
    // Constructors
    // ------------------------------------------------------------------

    /// Default-construct with all fields zero-initialised.
    TickData() : bid_price(0.0), ask_price(0.0), timestamp_ns(0) {
        std::memset(symbol, 0, SYMBOL_MAX_LEN);
        std::memset(_pad,   0, sizeof(_pad));
    }

    /**
     * Construct with explicit field values.
     * @param sym   Null-terminated instrument symbol (truncated to 15 chars).
     * @param bid   Best bid price.
     * @param ask   Best ask price.
     * @param ts_ns Nanosecond timestamp.
     */
    TickData(const char* sym, double bid, double ask, int64_t ts_ns)
        : bid_price(bid), ask_price(ask), timestamp_ns(ts_ns)
    {
        std::strncpy(symbol, sym, SYMBOL_MAX_LEN - 1);
        symbol[SYMBOL_MAX_LEN - 1] = '\0';
        std::memset(_pad, 0, sizeof(_pad));
    }

    // ------------------------------------------------------------------
    // Serialisation
    // ------------------------------------------------------------------

    /**
     * Serialise this tick to a compact JSON string.
     *
     * Output format:
     * @code
     * {"symbol":"RELIANCE","bid":2850.25,"ask":2850.75,"ts_ns":1737350400000000000}
     * @endcode
     *
     * @note JSON is chosen for TCP transport legibility; binary formats
     *       (Flatbuffers, SBE) would be faster and are a natural next step.
     */
    [[nodiscard]] std::string to_json() const {
        nlohmann::json j;
        j["symbol"] = symbol;
        j["bid"]    = bid_price;
        j["ask"]    = ask_price;
        j["ts_ns"]  = timestamp_ns;
        return j.dump();
    }

    /**
     * Deserialise a JSON string into @p out.
     * @return true on success, false on any parse or type error.
     *
     * Avoids exceptions in the hot path by catching internally and
     * returning a boolean status.
     */
    static bool from_json(const std::string& json_str, TickData& out) noexcept {
        try {
            auto j = nlohmann::json::parse(json_str);

            std::string sym = j.at("symbol").get<std::string>();
            std::strncpy(out.symbol, sym.c_str(), SYMBOL_MAX_LEN - 1);
            out.symbol[SYMBOL_MAX_LEN - 1] = '\0';

            out.bid_price    = j.at("bid").get<double>();
            out.ask_price    = j.at("ask").get<double>();
            out.timestamp_ns = j.at("ts_ns").get<int64_t>();
            return true;
        } catch (...) {
            return false;
        }
    }
};

// ---------------------------------------------------------------------------
// Static assertions (compile-time guarantees)
// ---------------------------------------------------------------------------

static_assert(sizeof(TickData)  == 64,
    "TickData must be exactly 64 bytes for cache-line alignment");
static_assert(alignof(TickData) == 64,
    "TickData must be aligned to 64-byte boundaries");
static_assert(sizeof(int64_t)   == 8,
    "int64_t must be 8 bytes for nanosecond timestamp range");

} // namespace mds
