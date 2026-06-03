#ifndef MD_LOG_READER_H
#define MD_LOG_READER_H

// Reader for SpscMdLogger files. Streams MD_EVENT records back into
// MarketDataEvent values. Validates the BinaryFraming header (magic /
// version / endianness) on open; throws on mismatch or truncation.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../MarketDataEvent.h"
#include "../Order.h"
#include "BinaryFraming.h"

class MdLogReader {
public:
    explicit MdLogReader(const std::string& path)
        : in_(path, std::ios::binary) {
        if (!in_) {
            throw std::runtime_error("MdLogReader: failed to open " + path);
        }
        mme_log::FileHeader hdr{};
        if (!in_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
            throw std::runtime_error("MdLogReader: truncated header in " + path);
        }
        if (hdr.magic != mme_log::kMagic) {
            throw std::runtime_error("MdLogReader: bad magic in " + path);
        }
        if (hdr.endian_sig != mme_log::kEndianSig) {
            throw std::runtime_error("MdLogReader: endianness mismatch in " + path);
        }
        if (hdr.version != mme_log::kVersion) {
            throw std::runtime_error("MdLogReader: unsupported version in " + path);
        }
    }

    // Returns nullopt at clean EOF; throws on a malformed / truncated record.
    std::optional<MarketDataEvent> next() {
        uint32_t payload_len = 0;
        uint8_t type_byte = 0;
        if (!in_.read(reinterpret_cast<char*>(&payload_len), sizeof(payload_len))) {
            if (in_.eof()) return std::nullopt;
            throw std::runtime_error("MdLogReader: truncated record length");
        }
        if (!in_.read(reinterpret_cast<char*>(&type_byte), sizeof(type_byte))) {
            throw std::runtime_error("MdLogReader: truncated record type");
        }
        // Only MD_EVENT is expected in an MD log; skip foreign records
        // defensively rather than mixing them in.
        if (static_cast<mme_log::RecordType>(type_byte) !=
            mme_log::RecordType::MD_EVENT) {
            in_.seekg(payload_len, std::ios::cur);
            return next();
        }
        scratch_.resize(payload_len);
        if (payload_len > 0 &&
            !in_.read(scratch_.data(), payload_len)) {
            throw std::runtime_error("MdLogReader: truncated record payload");
        }
        return decode(scratch_.data(), payload_len);
    }

private:
    static std::chrono::system_clock::time_point ts_from_ns(int64_t ns) {
        using sc = std::chrono::system_clock;
        return sc::time_point{
            std::chrono::duration_cast<sc::duration>(std::chrono::nanoseconds{ns})};
    }

    static MarketDataEvent decode(const char* base, std::size_t len) {
        const char* p = base;
        const char* end = base + len;
        MarketDataEvent ev;
        ev.sequence_number = mme_log::take<int64_t>(p);
        ev.timestamp = ts_from_ns(mme_log::take<int64_t>(p));
        ev.best_bid_price = static_cast<Ticks>(mme_log::take<int64_t>(p));
        ev.best_ask_price = static_cast<Ticks>(mme_log::take<int64_t>(p));
        ev.best_bid_size = mme_log::take<int32_t>(p);
        ev.best_ask_size = mme_log::take<int32_t>(p);
        ev.instrument = read_string(p);
        read_levels(p, ev.bid_levels);
        read_levels(p, ev.ask_levels);
        read_trades(p, ev.trades);
        read_partial_fills(p, ev.partial_fills);
        read_mm_fills(p, ev.mm_fills);
        if (p != end) {
            throw std::runtime_error("MdLogReader: payload trailing bytes");
        }
        return ev;
    }

    static std::string read_string(const char*& p) {
        const uint16_t n = mme_log::take<uint16_t>(p);
        std::string s(p, p + n);
        p += n;
        return s;
    }

    static void read_levels(const char*& p, std::vector<OrderLevel>& out) {
        const uint16_t n = mme_log::take<uint16_t>(p);
        out.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            const int64_t price = mme_log::take<int64_t>(p);
            const int32_t size = mme_log::take<int32_t>(p);
            const uint64_t oid  = mme_log::take<uint64_t>(p);
            const int64_t ts   = mme_log::take<int64_t>(p);
            out.emplace_back(static_cast<Ticks>(price), size, oid, ts_from_ns(ts));
        }
    }

    static void read_trades(const char*& p, std::vector<Trade>& out) {
        const uint16_t n = mme_log::take<uint16_t>(p);
        out.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            Trade t;
            t.aggressor_side = mme_log::take<uint8_t>(p) == 0 ? Side::BUY : Side::SELL;
            t.price = static_cast<Ticks>(mme_log::take<int64_t>(p));
            t.size = mme_log::take<int32_t>(p);
            t.trade_id = mme_log::take<uint64_t>(p);
            t.timestamp = ts_from_ns(mme_log::take<int64_t>(p));
            out.push_back(t);
        }
    }

    static void read_partial_fills(const char*& p, std::vector<PartialFillEvent>& out) {
        const uint16_t n = mme_log::take<uint16_t>(p);
        out.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            PartialFillEvent f;
            f.order_id = mme_log::take<uint64_t>(p);
            f.price = static_cast<Ticks>(mme_log::take<int64_t>(p));
            f.filled_size = mme_log::take<int32_t>(p);
            f.remaining_size = mme_log::take<int32_t>(p);
            f.timestamp = ts_from_ns(mme_log::take<int64_t>(p));
            out.push_back(f);
        }
    }

    static void read_mm_fills(const char*& p, std::vector<FillEvent>& out) {
        const uint16_t n = mme_log::take<uint16_t>(p);
        out.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            FillEvent f;
            f.order_id = mme_log::take<uint64_t>(p);
            f.trade_id = mme_log::take<uint64_t>(p);
            f.side = mme_log::take<uint8_t>(p) == 0 ? Side::BUY : Side::SELL;
            f.price = static_cast<Ticks>(mme_log::take<int64_t>(p));
            f.fill_qty = mme_log::take<int32_t>(p);
            f.leaves_qty = mme_log::take<int32_t>(p);
            f.timestamp = ts_from_ns(mme_log::take<int64_t>(p));
            out.push_back(f);
        }
    }

    std::ifstream in_;
    std::vector<char> scratch_;
};

#endif  // MD_LOG_READER_H
