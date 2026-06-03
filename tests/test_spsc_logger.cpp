// SpscLogger end-to-end: write known engine events through the Logger
// interface, drop the logger (forces drain + flush), then read the file
// back manually using the BinaryFraming helpers and verify record-by-
// record. Verifies the header, type tags, and payload layouts.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "include/BinaryFraming.h"
#include "include/Instrument.h"
#include "include/SpscLogger.h"
#include "Order.h"

namespace {

std::chrono::system_clock::time_point at_ns(int64_t ns) {
    using sc = std::chrono::system_clock;
    return sc::time_point{
        std::chrono::duration_cast<sc::duration>(std::chrono::nanoseconds{ns})};
}

std::vector<char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
}

struct Record {
    mme_log::RecordType type;
    std::vector<char> payload;
};

std::vector<Record> parse_file(const std::vector<char>& bytes) {
    std::vector<Record> out;
    const char* p = bytes.data();
    const char* end = bytes.data() + bytes.size();
    // Header
    EXPECT_GE(static_cast<std::size_t>(end - p), mme_log::kHeaderBytes);
    mme_log::FileHeader hdr{};
    std::memcpy(&hdr, p, sizeof(hdr));
    p += sizeof(hdr);
    EXPECT_EQ(hdr.magic, mme_log::kMagic);
    EXPECT_EQ(hdr.version, mme_log::kVersion);
    EXPECT_EQ(hdr.endian_sig, mme_log::kEndianSig);
    while (p < end) {
        uint32_t len = mme_log::take<uint32_t>(p);
        uint8_t type = mme_log::take<uint8_t>(p);
        Record r;
        r.type = static_cast<mme_log::RecordType>(type);
        r.payload.assign(p, p + len);
        p += len;
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace

TEST(SpscLogger, RoundTripsAllRecordTypes) {
    const auto path = (std::filesystem::temp_directory_path()
                       / "spsc_logger_roundtrip.bin").string();
    std::remove(path.c_str());

    const Instrument ins(0.01);
    FillEvent fill{
        /*order_id*/ 42,
        /*trade_id*/ 1001,
        /*side*/     Side::SELL,
        /*price*/    static_cast<Ticks>(10005),  // 100.05
        /*fill_qty*/ 3,
        /*leaves_qty*/ 2,
        /*timestamp*/ at_ns(123'456'789'000LL),
    };

    {
        SpscLogger logger(path, /*capacity*/ 4096);
        logger.on_fill(fill, ins, /*pos*/ -3,
                       /*cash*/  300.15,
                       /*real*/  -1.25,
                       /*unreal*/ 0.75);
        logger.on_sequence_gap(7);
        logger.on_empty_book();
        logger.on_amend_rejected(99);
        // dtor joins drain thread + flushes
    }

    const auto bytes = read_file(path);
    const auto records = parse_file(bytes);
    ASSERT_EQ(records.size(), 4u);

    // FILL
    EXPECT_EQ(records[0].type, mme_log::RecordType::FILL);
    {
        const char* p = records[0].payload.data();
        EXPECT_EQ(mme_log::take<uint64_t>(p), 42u);
        EXPECT_EQ(mme_log::take<uint64_t>(p), 1001u);
        EXPECT_EQ(mme_log::take<uint8_t>(p), 1u);  // SELL
        EXPECT_EQ(mme_log::take<int64_t>(p), 10005LL);
        EXPECT_EQ(mme_log::take<int32_t>(p), 3);
        EXPECT_EQ(mme_log::take<int32_t>(p), 2);
        EXPECT_EQ(mme_log::take<int32_t>(p), -3);
        EXPECT_DOUBLE_EQ(mme_log::take<double>(p), 300.15);
        EXPECT_DOUBLE_EQ(mme_log::take<double>(p), -1.25);
        EXPECT_DOUBLE_EQ(mme_log::take<double>(p), 0.75);
        EXPECT_DOUBLE_EQ(mme_log::take<double>(p), 0.01);
        EXPECT_EQ(mme_log::take<int64_t>(p), 123'456'789'000LL);
    }

    // SEQ_GAP
    EXPECT_EQ(records[1].type, mme_log::RecordType::SEQ_GAP);
    {
        const char* p = records[1].payload.data();
        EXPECT_EQ(mme_log::take<int64_t>(p), 7LL);
    }

    // EMPTY_BOOK
    EXPECT_EQ(records[2].type, mme_log::RecordType::EMPTY_BOOK);
    EXPECT_TRUE(records[2].payload.empty());

    // AMEND_REJECT
    EXPECT_EQ(records[3].type, mme_log::RecordType::AMEND_REJECT);
    {
        const char* p = records[3].payload.data();
        EXPECT_EQ(mme_log::take<uint64_t>(p), 99u);
    }

    std::remove(path.c_str());
}

TEST(SpscLogger, ManyRecordsDoNotDrop) {
    // 10k records, each ~57 bytes (FILL framed), into a 1 MiB ring. Drain
    // thread runs concurrently. Expect every record to land in the file.
    const auto path = (std::filesystem::temp_directory_path()
                       / "spsc_logger_many.bin").string();
    std::remove(path.c_str());

    const Instrument ins(0.01);
    constexpr int kCount = 10'000;

    {
        SpscLogger logger(path);
        FillEvent f{0, 0, Side::BUY, 0, 1, 0, at_ns(0)};
        for (int i = 0; i < kCount; ++i) {
            f.order_id = static_cast<uint64_t>(i);
            f.trade_id = static_cast<uint64_t>(i) + 1;
            f.price    = static_cast<Ticks>(10000 + (i % 10));
            f.timestamp = at_ns(static_cast<int64_t>(i) * 1000);
            logger.on_fill(f, ins, /*pos*/ i, 0.0, 0.0, 0.0);
        }
        EXPECT_EQ(logger.drops(), 0u);
    }

    const auto records = parse_file(read_file(path));
    ASSERT_EQ(records.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(records[i].type, mme_log::RecordType::FILL);
        const char* p = records[i].payload.data();
        EXPECT_EQ(mme_log::take<uint64_t>(p), static_cast<uint64_t>(i));
    }

    std::remove(path.c_str());
}
