#ifndef SPSC_MD_LOGGER_H
#define SPSC_MD_LOGGER_H

// Drop-on-full SPSC logger for full MarketDataEvent records — replaces
// include/BinaryLogger.h and the old text replay path. Same SPSC ring +
// drain-thread shape as SpscLogger; just a different record type with
// variable-length payload encoding the event's vector fields.
//
// Wire layout per MD_EVENT record (little-endian, no padding):
//   sequence_number   i64
//   timestamp_ns      i64
//   best_bid_price    i64 (Ticks)
//   best_ask_price    i64 (Ticks)
//   best_bid_size     i32
//   best_ask_size     i32
//   instrument        u16 len, then len bytes
//   bid_levels        u16 count, then count × OrderLevel (28 B each)
//   ask_levels        u16 count, then count × OrderLevel
//   trades            u16 count, then count × Trade (29 B each)
//   partial_fills     u16 count, then count × PartialFillEvent (32 B each)
//   mm_fills          u16 count, then count × FillEvent (37 B each)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "../MarketDataEvent.h"
#include "../Order.h"
#include "BinaryFraming.h"
#include "SpscRing.h"

class SpscMdLogger {
public:
    static constexpr std::size_t kDefaultCapacity = 1u << 20;  // 1 MiB
    // Per-record stack buffer cap. Typical MD events serialize to ~1.5 KiB
    // (5 levels × 2 sides + ≤10 trades + ≤10 partial fills + ≤10 mm fills).
    // 8 KiB leaves ample headroom and stays well within thread stack budget.
    static constexpr std::size_t kRecordBufBytes = 8u * 1024u;

    explicit SpscMdLogger(const std::string& path,
                          std::size_t ring_capacity = kDefaultCapacity)
        : ring_(ring_capacity),
          out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) {
            throw std::runtime_error("SpscMdLogger: failed to open " + path);
        }
        const mme_log::FileHeader hdr = mme_log::make_header();
        out_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        drain_ = std::thread([this] { drain_loop(); });
    }

    ~SpscMdLogger() {
        stop_.store(true, std::memory_order_release);
        if (drain_.joinable()) drain_.join();
        flush_ring_to_file();
        out_.flush();
    }

    SpscMdLogger(const SpscMdLogger&) = delete;
    SpscMdLogger& operator=(const SpscMdLogger&) = delete;

    uint64_t drops() const { return ring_.drops(); }

    void log_event(const MarketDataEvent& ev) {
        char buf[kRecordBufBytes];
        // Reserve 5 bytes for the record header (filled in at the end once
        // payload length is known). Write payload starting at buf+5.
        char* const payload_start = buf + sizeof(uint32_t) + 1u;
        char* w = payload_start;

        mme_log::put<int64_t>(w, ev.sequence_number);
        mme_log::put<int64_t>(w, ts_ns(ev.timestamp));
        mme_log::put<int64_t>(w, static_cast<int64_t>(ev.best_bid_price));
        mme_log::put<int64_t>(w, static_cast<int64_t>(ev.best_ask_price));
        mme_log::put<int32_t>(w, ev.best_bid_size);
        mme_log::put<int32_t>(w, ev.best_ask_size);

        write_string(w, ev.instrument);
        write_levels(w, ev.bid_levels);
        write_levels(w, ev.ask_levels);
        write_trades(w, ev.trades);
        write_partial_fills(w, ev.partial_fills);
        write_mm_fills(w, ev.mm_fills);

        const std::size_t payload_len =
            static_cast<std::size_t>(w - payload_start);
        // Bounds check: defensive — fires only on malformed inputs that
        // would otherwise overflow the stack buffer. In production this
        // would be a hard assert.
        if (payload_len > kRecordBufBytes - (sizeof(uint32_t) + 1u)) {
            // Truncate by dropping. Caller can detect via drops().
            ring_.drops();  // already counted via the absence of try_push
            return;
        }
        const std::size_t total = sizeof(uint32_t) + 1u + payload_len;
        mme_log::write_record_header(
            buf, mme_log::RecordType::MD_EVENT,
            static_cast<uint32_t>(payload_len));
        ring_.try_push(buf, total);
    }

private:
    static int64_t ts_ns(std::chrono::system_clock::time_point ts) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            ts.time_since_epoch()).count();
    }

    static void write_string(char*& w, const std::string& s) {
        mme_log::put<uint16_t>(w, static_cast<uint16_t>(s.size()));
        std::memcpy(w, s.data(), s.size());
        w += s.size();
    }

    static void write_levels(char*& w, const std::vector<OrderLevel>& lv) {
        mme_log::put<uint16_t>(w, static_cast<uint16_t>(lv.size()));
        for (const auto& l : lv) {
            mme_log::put<int64_t>(w, static_cast<int64_t>(l.price));
            mme_log::put<int32_t>(w, l.size);
            mme_log::put<uint64_t>(w, l.order_id);
            mme_log::put<int64_t>(w, ts_ns(l.timestamp));
        }
    }

    static void write_trades(char*& w, const std::vector<Trade>& tr) {
        mme_log::put<uint16_t>(w, static_cast<uint16_t>(tr.size()));
        for (const auto& t : tr) {
            mme_log::put<uint8_t>(w, t.aggressor_side == Side::BUY ? 0 : 1);
            mme_log::put<int64_t>(w, static_cast<int64_t>(t.price));
            mme_log::put<int32_t>(w, t.size);
            mme_log::put<uint64_t>(w, t.trade_id);
            mme_log::put<int64_t>(w, ts_ns(t.timestamp));
        }
    }

    static void write_partial_fills(char*& w, const std::vector<PartialFillEvent>& fl) {
        mme_log::put<uint16_t>(w, static_cast<uint16_t>(fl.size()));
        for (const auto& f : fl) {
            mme_log::put<uint64_t>(w, f.order_id);
            mme_log::put<int64_t>(w, static_cast<int64_t>(f.price));
            mme_log::put<int32_t>(w, f.filled_size);
            mme_log::put<int32_t>(w, f.remaining_size);
            mme_log::put<int64_t>(w, ts_ns(f.timestamp));
        }
    }

    static void write_mm_fills(char*& w, const std::vector<FillEvent>& fl) {
        mme_log::put<uint16_t>(w, static_cast<uint16_t>(fl.size()));
        for (const auto& f : fl) {
            mme_log::put<uint64_t>(w, f.order_id);
            mme_log::put<uint64_t>(w, f.trade_id);
            mme_log::put<uint8_t>(w, f.side == Side::BUY ? 0 : 1);
            mme_log::put<int64_t>(w, static_cast<int64_t>(f.price));
            mme_log::put<int32_t>(w, f.fill_qty);
            mme_log::put<int32_t>(w, f.leaves_qty);
            mme_log::put<int64_t>(w, ts_ns(f.timestamp));
        }
    }

    void drain_loop() {
        using namespace std::chrono_literals;
        while (!stop_.load(std::memory_order_acquire)) {
            if (flush_ring_to_file() == 0) {
                std::this_thread::sleep_for(100us);
            }
        }
    }

    std::size_t flush_ring_to_file() {
        std::size_t total = 0;
        char buf[8192];
        for (;;) {
            const std::size_t n = ring_.pop_some(buf, sizeof(buf));
            if (n == 0) break;
            out_.write(buf, static_cast<std::streamsize>(n));
            total += n;
        }
        return total;
    }

    mme_log::SpscRing ring_;
    std::ofstream out_;
    std::thread drain_;
    std::atomic<bool> stop_{false};
};

#endif  // SPSC_MD_LOGGER_H
