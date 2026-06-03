// Unit tests for the byte-oriented SPSC ring + BinaryFraming helpers.
// Covers: wrap-around, drop-on-full, multi-threaded SPSC under contention,
// and round-trip of the scalar put/take helpers + framing header.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "include/BinaryFraming.h"
#include "include/SpscRing.h"

using mme_log::SpscRing;

TEST(SpscRing, RejectsNonPowerOfTwoCapacity) {
    EXPECT_THROW(SpscRing(100), std::invalid_argument);
    EXPECT_THROW(SpscRing(0), std::invalid_argument);
    EXPECT_THROW(SpscRing(32), std::invalid_argument);  // < 64
    EXPECT_NO_THROW(SpscRing(64));
    EXPECT_NO_THROW(SpscRing(1024));
}

TEST(SpscRing, PushAndPopRoundTrip) {
    SpscRing ring(256);
    const char msg[] = "hello-spsc-ring";
    ASSERT_TRUE(ring.try_push(msg, sizeof(msg)));
    EXPECT_EQ(ring.readable(), sizeof(msg));

    char out[sizeof(msg)] = {};
    EXPECT_EQ(ring.pop_some(out, sizeof(out)), sizeof(msg));
    EXPECT_EQ(std::memcmp(out, msg, sizeof(msg)), 0);
    EXPECT_EQ(ring.readable(), 0u);
}

TEST(SpscRing, DropsWhenFull) {
    SpscRing ring(64);
    std::vector<char> filler(64, 'x');
    ASSERT_TRUE(ring.try_push(filler.data(), filler.size()));
    EXPECT_FALSE(ring.try_push("x", 1));
    EXPECT_EQ(ring.drops(), 1u);

    // After draining, the ring accepts new writes again.
    char buf[64];
    EXPECT_EQ(ring.pop_some(buf, sizeof(buf)), 64u);
    EXPECT_TRUE(ring.try_push("ok", 2));
    EXPECT_EQ(ring.readable(), 2u);
}

TEST(SpscRing, WrapAroundPreservesBytes) {
    SpscRing ring(64);
    // Fill 40 bytes, drain them — moves head and tail both to 40, so the
    // next push of 40 bytes will straddle the wrap point.
    std::vector<char> chunk_a(40);
    for (std::size_t i = 0; i < chunk_a.size(); ++i) chunk_a[i] = static_cast<char>(i);
    ASSERT_TRUE(ring.try_push(chunk_a.data(), chunk_a.size()));
    char tmp[40];
    ASSERT_EQ(ring.pop_some(tmp, sizeof(tmp)), 40u);

    // Now push 40 bytes again — physical offset starts at 40, capacity is
    // 64, so 24 bytes go in the tail of the buffer and 16 wrap to the front.
    std::vector<char> chunk_b(40);
    for (std::size_t i = 0; i < chunk_b.size(); ++i) chunk_b[i] = static_cast<char>(0x80 | i);
    ASSERT_TRUE(ring.try_push(chunk_b.data(), chunk_b.size()));

    char out[40] = {};
    ASSERT_EQ(ring.pop_some(out, sizeof(out)), 40u);
    EXPECT_EQ(std::memcmp(out, chunk_b.data(), chunk_b.size()), 0);
}

TEST(SpscRing, MultiThreadedSpscRoundTrip) {
    // One producer thread pushes a known sequence of varying-size records;
    // one consumer thread drains them and reconstructs the original stream.
    SpscRing ring(4096);
    constexpr int kRecords = 100'000;

    std::vector<std::vector<char>> records;
    records.reserve(kRecords);
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> size_dist(8, 200);
    for (int i = 0; i < kRecords; ++i) {
        const int n = size_dist(rng);
        std::vector<char> r(static_cast<std::size_t>(n));
        for (int j = 0; j < n; ++j) {
            r[static_cast<std::size_t>(j)] = static_cast<char>((i * 31 + j) & 0xFF);
        }
        records.push_back(std::move(r));
    }

    std::atomic<bool> producer_done{false};
    std::vector<char> consumed;
    consumed.reserve(kRecords * 100);

    std::thread consumer([&] {
        char buf[512];
        while (true) {
            const std::size_t got = ring.pop_some(buf, sizeof(buf));
            if (got > 0) {
                consumed.insert(consumed.end(), buf, buf + got);
            } else if (producer_done.load(std::memory_order_acquire)
                       && ring.readable() == 0) {
                break;
            }
        }
    });

    std::thread producer([&] {
        for (const auto& r : records) {
            while (!ring.try_push(r.data(), r.size())) {
                // Spin on backpressure — ring is sized to absorb but the
                // consumer schedule isn't guaranteed.
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    // Drops count failed try_push attempts; the producer spins on backpressure
    // and retries with the same bytes, so the consumed stream is still
    // byte-equal to the produced stream regardless of drop count.

    std::vector<char> expected;
    for (const auto& r : records) expected.insert(expected.end(), r.begin(), r.end());
    ASSERT_EQ(consumed.size(), expected.size());
    EXPECT_EQ(std::memcmp(consumed.data(), expected.data(), expected.size()), 0);
}

TEST(BinaryFraming, ScalarPutTakeRoundTrip) {
    char buf[64];
    char* w = buf;
    mme_log::put<uint32_t>(w, 0xDEADBEEF);
    mme_log::put<int64_t>(w, -123456789LL);
    mme_log::put<double>(w, 3.14159265358979);
    mme_log::put<uint8_t>(w, 0xAB);

    const char* r = buf;
    EXPECT_EQ(mme_log::take<uint32_t>(r), 0xDEADBEEFu);
    EXPECT_EQ(mme_log::take<int64_t>(r), -123456789LL);
    EXPECT_DOUBLE_EQ(mme_log::take<double>(r), 3.14159265358979);
    EXPECT_EQ(mme_log::take<uint8_t>(r), 0xABu);
}

TEST(BinaryFraming, HeaderConstants) {
    const mme_log::FileHeader h = mme_log::make_header();
    EXPECT_EQ(h.magic, mme_log::kMagic);
    EXPECT_EQ(h.version, mme_log::kVersion);
    EXPECT_EQ(h.endian_sig, mme_log::kEndianSig);
    EXPECT_EQ(h.reserved, 0u);
    EXPECT_EQ(sizeof(h), mme_log::kHeaderBytes);
}

TEST(BinaryFraming, RecordHeaderWriteAndSize) {
    EXPECT_EQ(mme_log::framed_size(0), 5u);
    EXPECT_EQ(mme_log::framed_size(42), 47u);

    char buf[16];
    char* payload = mme_log::write_record_header(
        buf, mme_log::RecordType::FILL, 8);
    EXPECT_EQ(payload, buf + 5);

    uint32_t len = 0;
    std::memcpy(&len, buf, sizeof(len));
    EXPECT_EQ(len, 8u);
    EXPECT_EQ(static_cast<uint8_t>(buf[4]),
              static_cast<uint8_t>(mme_log::RecordType::FILL));
}
