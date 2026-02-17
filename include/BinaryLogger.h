#ifndef BINARY_LOGGER_H
#define BINARY_LOGGER_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <chrono>
#include "../MarketDataEvent.h"

class BinaryLogger {
public:
    explicit BinaryLogger(const std::string& path)
        : out_(path, std::ios::binary | std::ios::trunc) {}

    bool is_open() const { return out_.is_open(); }

    void log_event(const MarketDataEvent& ev) {
        // Build record into buffer, then write total_len prefix + payload.
        buf_.clear();

        // Reserve space for total_len (filled in at the end)
        append<uint32_t>(0);

        append<int64_t>(ev.sequence_number);
        int64_t ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            ev.timestamp.time_since_epoch()).count();
        append<int64_t>(ts_ns);
        append<double>(ev.best_bid_price);
        append<double>(ev.best_ask_price);
        append<int32_t>(ev.best_bid_size);
        append<int32_t>(ev.best_ask_size);
        append<uint16_t>(static_cast<uint16_t>(ev.trades.size()));
        append<uint16_t>(static_cast<uint16_t>(ev.partial_fills.size()));

        for (const auto& t : ev.trades) {
            append<uint8_t>(t.aggressor_side == Side::BUY ? 1 : 0);
            append<double>(t.price);
            append<int32_t>(t.size);
            append<uint64_t>(t.trade_id);
        }

        for (const auto& f : ev.partial_fills) {
            append<uint64_t>(f.order_id);
            append<double>(f.price);
            append<int32_t>(f.filled_size);
            append<int32_t>(f.remaining_size);
        }

        // Patch total_len at the start
        uint32_t total_len = static_cast<uint32_t>(buf_.size());
        std::memcpy(buf_.data(), &total_len, sizeof(total_len));

        out_.write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
    }

    void flush() { out_.flush(); }

private:
    std::ofstream out_;
    std::vector<char> buf_;

    template <typename T>
    void append(T value) {
        const char* p = reinterpret_cast<const char*>(&value);
        buf_.insert(buf_.end(), p, p + sizeof(T));
    }
};

#endif // BINARY_LOGGER_H
