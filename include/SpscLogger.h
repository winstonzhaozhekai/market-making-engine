#ifndef SPSC_LOGGER_H
#define SPSC_LOGGER_H

// Drop-on-full SPSC logger backing the M6 C3 closure. Implements the
// Logger interface; producer (MarketMaker hot path) encodes scalar-only
// records onto a fixed-size stack buffer and pushes the bytes into an
// SpscRing — no allocation, no I/O, no formatting on the producer thread.
// A dedicated drain thread pops the bytes and writes them to a binary
// file. Header + record framing per include/BinaryFraming.h.
//
// Wire layout per record:
//   FILL          payload (52 B): order_id u64, trade_id u64, side u8,
//                                 price_ticks i64, fill_qty i32,
//                                 leaves_qty i32, position i32,
//                                 cash f64, realized f64, unrealized f64,
//                                 tick_size f64, timestamp_ns i64
//   SEQ_GAP       payload (8 B):  missed i64
//   EMPTY_BOOK    payload (0 B)
//   AMEND_REJECT  payload (8 B):  order_id u64

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "BinaryFraming.h"
#include "Instrument.h"
#include "Logger.h"
#include "../Order.h"
#include "SpscRing.h"

class SpscLogger final : public Logger {
public:
    // Default capacity is 1 MiB, sized for typical MM rates with plenty of
    // headroom; caller can shrink for tests.
    static constexpr std::size_t kDefaultCapacity = 1u << 20;

    explicit SpscLogger(const std::string& path,
                        std::size_t ring_capacity = kDefaultCapacity)
        : ring_(ring_capacity),
          out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) {
            throw std::runtime_error("SpscLogger: failed to open " + path);
        }
        const mme_log::FileHeader hdr = mme_log::make_header();
        out_.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        drain_ = std::thread([this] { drain_loop(); });
    }

    ~SpscLogger() override {
        stop_.store(true, std::memory_order_release);
        if (drain_.joinable()) drain_.join();
        // Final drain pass for anything the loop missed between its last
        // check and the stop_ store.
        flush_ring_to_file();
        out_.flush();
    }

    SpscLogger(const SpscLogger&) = delete;
    SpscLogger& operator=(const SpscLogger&) = delete;

    uint64_t drops() const { return ring_.drops(); }

    void on_fill(const FillEvent& fill, const Instrument& ins,
                 int position, double cash,
                 double realized_pnl, double unrealized_pnl) override {
        constexpr uint32_t kPayload = 8 + 8 + 1 + 8 + 4 + 4 + 4 + 8 + 8 + 8 + 8 + 8;
        char buf[mme_log::framed_size(kPayload)];
        char* w = mme_log::write_record_header(buf, mme_log::RecordType::FILL, kPayload);
        mme_log::put<uint64_t>(w, fill.order_id);
        mme_log::put<uint64_t>(w, fill.trade_id);
        mme_log::put<uint8_t>(w, fill.side == Side::BUY ? 0 : 1);
        mme_log::put<int64_t>(w, static_cast<int64_t>(fill.price));
        mme_log::put<int32_t>(w, fill.fill_qty);
        mme_log::put<int32_t>(w, fill.leaves_qty);
        mme_log::put<int32_t>(w, position);
        mme_log::put<double>(w, cash);
        mme_log::put<double>(w, realized_pnl);
        mme_log::put<double>(w, unrealized_pnl);
        mme_log::put<double>(w, ins.tick_size);
        mme_log::put<int64_t>(w, ts_ns(fill.timestamp));
        ring_.try_push(buf, sizeof(buf));
    }

    void on_sequence_gap(int64_t missed) override {
        constexpr uint32_t kPayload = 8;
        char buf[mme_log::framed_size(kPayload)];
        char* w = mme_log::write_record_header(buf, mme_log::RecordType::SEQ_GAP, kPayload);
        mme_log::put<int64_t>(w, missed);
        ring_.try_push(buf, sizeof(buf));
    }

    void on_empty_book() override {
        constexpr uint32_t kPayload = 0;
        char buf[mme_log::framed_size(kPayload)];
        mme_log::write_record_header(buf, mme_log::RecordType::EMPTY_BOOK, kPayload);
        ring_.try_push(buf, sizeof(buf));
    }

    void on_amend_rejected(uint64_t order_id) override {
        constexpr uint32_t kPayload = 8;
        char buf[mme_log::framed_size(kPayload)];
        char* w = mme_log::write_record_header(buf, mme_log::RecordType::AMEND_REJECT, kPayload);
        mme_log::put<uint64_t>(w, order_id);
        ring_.try_push(buf, sizeof(buf));
    }

private:
    static int64_t ts_ns(std::chrono::system_clock::time_point ts) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            ts.time_since_epoch()).count();
    }

    void drain_loop() {
        // Polled-sleep design: no condvar, no notify on the producer side.
        // 100us is short enough that the ring won't fill under typical MM
        // rates and long enough that we're not spinning on the cache line.
        using namespace std::chrono_literals;
        while (!stop_.load(std::memory_order_acquire)) {
            if (flush_ring_to_file() == 0) {
                std::this_thread::sleep_for(100us);
            }
        }
    }

    std::size_t flush_ring_to_file() {
        std::size_t total = 0;
        char buf[4096];
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

#endif  // SPSC_LOGGER_H
