#include "include/ItchParser.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace itch = mme::itch;

namespace {

itch::CommonHeader make_hdr(std::uint16_t locate, std::uint64_t ts_ns) {
    return { locate, 0xBEEF, ts_ns };
}

}  // namespace

// ---- Length table ---------------------------------------------------

TEST(ItchLengths, NasdaqV5Reference) {
    EXPECT_EQ(itch::message_length('S'), 12u);
    EXPECT_EQ(itch::message_length('R'), 39u);
    EXPECT_EQ(itch::message_length('H'), 25u);
    EXPECT_EQ(itch::message_length('A'), 36u);
    EXPECT_EQ(itch::message_length('F'), 40u);
    EXPECT_EQ(itch::message_length('E'), 31u);
    EXPECT_EQ(itch::message_length('C'), 36u);
    EXPECT_EQ(itch::message_length('X'), 23u);
    EXPECT_EQ(itch::message_length('D'), 19u);
    EXPECT_EQ(itch::message_length('U'), 35u);
    EXPECT_EQ(itch::message_length('P'), 44u);
    EXPECT_EQ(itch::message_length('Q'), 40u);
    EXPECT_EQ(itch::message_length('B'), 19u);
    EXPECT_EQ(itch::message_length('I'), 50u);
}

TEST(ItchLengths, UnknownTypeReturnsZero) {
    EXPECT_EQ(itch::message_length('\0'), 0u);
    EXPECT_EQ(itch::message_length('z'), 0u);
}

// ---- Big-endian primitive round-trip --------------------------------

TEST(ItchEndian, BE16RoundTrip) {
    std::uint8_t buf[2];
    itch::write_be16(buf, 0xABCD);
    EXPECT_EQ(buf[0], 0xAB);
    EXPECT_EQ(buf[1], 0xCD);
    EXPECT_EQ(itch::read_be16(buf), 0xABCDu);
}

TEST(ItchEndian, BE32RoundTrip) {
    std::uint8_t buf[4];
    itch::write_be32(buf, 0x12345678u);
    EXPECT_EQ(buf[0], 0x12);
    EXPECT_EQ(buf[3], 0x78);
    EXPECT_EQ(itch::read_be32(buf), 0x12345678u);
}

TEST(ItchEndian, BE48RoundTrip) {
    std::uint8_t buf[6];
    // Largest 48-bit value: 2^48 - 1 = 281474976710655
    itch::write_be48(buf, 281474976710655ULL);
    EXPECT_EQ(buf[0], 0xFF);
    EXPECT_EQ(buf[5], 0xFF);
    EXPECT_EQ(itch::read_be48(buf), 281474976710655ULL);

    itch::write_be48(buf, 0x000123456789ULL);
    EXPECT_EQ(itch::read_be48(buf), 0x000123456789ULL);
}

TEST(ItchEndian, BE64RoundTrip) {
    std::uint8_t buf[8];
    itch::write_be64(buf, 0x0123456789ABCDEFULL);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[7], 0xEF);
    EXPECT_EQ(itch::read_be64(buf), 0x0123456789ABCDEFULL);
}

// ---- Per-type encode/decode round-trip ------------------------------

TEST(ItchRoundTrip, SystemEvent) {
    itch::SystemEvent in{ make_hdr(0, 1'000'000ULL), 'O' };
    std::uint8_t buf[12];
    const std::size_t n = itch::encode_system_event(buf, in);
    ASSERT_EQ(n, itch::message_length('S'));
    EXPECT_EQ(buf[0], 'S');

    const auto out = itch::decode_system_event(buf + 1);
    EXPECT_EQ(out.hdr.stock_locate, in.hdr.stock_locate);
    EXPECT_EQ(out.hdr.tracking_number, in.hdr.tracking_number);
    EXPECT_EQ(out.hdr.timestamp_ns, in.hdr.timestamp_ns);
    EXPECT_EQ(out.event_code, 'O');
}

TEST(ItchRoundTrip, StockDirectory) {
    itch::StockDirectory in{};
    in.hdr = make_hdr(42, 1'234'567'890ULL);
    std::memcpy(in.stock, "AAPL    ", 8);

    std::uint8_t buf[39];
    const std::size_t n = itch::encode_stock_directory(buf, in);
    ASSERT_EQ(n, itch::message_length('R'));
    EXPECT_EQ(buf[0], 'R');

    const auto out = itch::decode_stock_directory(buf + 1);
    EXPECT_EQ(out.hdr.stock_locate, 42u);
    EXPECT_EQ(out.hdr.timestamp_ns, 1'234'567'890ULL);
    EXPECT_EQ(std::memcmp(out.stock, "AAPL    ", 8), 0);
}

TEST(ItchRoundTrip, AddOrder) {
    itch::AddOrder in{};
    in.hdr           = make_hdr(7, 9'876'543'210ULL);
    in.order_ref     = 0x0001234567890ABCULL;
    in.side          = 'B';
    in.shares        = 100;
    std::memcpy(in.stock, "XYZ     ", 8);
    in.price_x10000  = 1'005'500;  // $100.55

    std::uint8_t buf[36];
    const std::size_t n = itch::encode_add_order(buf, in);
    ASSERT_EQ(n, itch::message_length('A'));
    EXPECT_EQ(buf[0], 'A');

    const auto out = itch::decode_add_order(buf + 1);
    EXPECT_EQ(out.hdr.stock_locate, in.hdr.stock_locate);
    EXPECT_EQ(out.hdr.timestamp_ns, in.hdr.timestamp_ns);
    EXPECT_EQ(out.order_ref, in.order_ref);
    EXPECT_EQ(out.side, 'B');
    EXPECT_EQ(out.shares, 100u);
    EXPECT_EQ(std::memcmp(out.stock, "XYZ     ", 8), 0);
    EXPECT_EQ(out.price_x10000, 1'005'500u);
}

TEST(ItchRoundTrip, AddOrderAttributed) {
    itch::AddOrderAttributed in{};
    in.hdr           = make_hdr(8, 1'000ULL);
    in.order_ref     = 12345ULL;
    in.side          = 'S';
    in.shares        = 200;
    std::memcpy(in.stock, "MSFT    ", 8);
    std::memcpy(in.attribution, "NSDQ", 4);
    in.price_x10000  = 2'500'000;

    std::uint8_t buf[40];
    const std::size_t n = itch::encode_add_order_attributed(buf, in);
    ASSERT_EQ(n, itch::message_length('F'));
    EXPECT_EQ(buf[0], 'F');

    const auto out = itch::decode_add_order_attributed(buf + 1);
    EXPECT_EQ(out.order_ref, 12345ULL);
    EXPECT_EQ(out.side, 'S');
    EXPECT_EQ(out.shares, 200u);
    EXPECT_EQ(std::memcmp(out.attribution, "NSDQ", 4), 0);
    EXPECT_EQ(out.price_x10000, 2'500'000u);
}

TEST(ItchRoundTrip, OrderExecuted) {
    itch::OrderExecuted in{ make_hdr(1, 42ULL), 99ULL, 30, 0xDEADBEEFCAFEULL };
    std::uint8_t buf[31];
    const std::size_t n = itch::encode_order_executed(buf, in);
    ASSERT_EQ(n, itch::message_length('E'));
    EXPECT_EQ(buf[0], 'E');

    const auto out = itch::decode_order_executed(buf + 1);
    EXPECT_EQ(out.order_ref, 99ULL);
    EXPECT_EQ(out.executed_shares, 30u);
    EXPECT_EQ(out.match_number, 0xDEADBEEFCAFEULL);
}

TEST(ItchRoundTrip, OrderExecutedWithPrice) {
    itch::OrderExecutedWithPrice in{
        make_hdr(1, 42ULL), 99ULL, 25, 7ULL, 'Y', 1'234'500u };
    std::uint8_t buf[36];
    const std::size_t n = itch::encode_order_executed_with_price(buf, in);
    ASSERT_EQ(n, itch::message_length('C'));
    EXPECT_EQ(buf[0], 'C');

    const auto out = itch::decode_order_executed_with_price(buf + 1);
    EXPECT_EQ(out.order_ref, 99ULL);
    EXPECT_EQ(out.executed_shares, 25u);
    EXPECT_EQ(out.match_number, 7ULL);
    EXPECT_EQ(out.printable, 'Y');
    EXPECT_EQ(out.execution_price_x10000, 1'234'500u);
}

TEST(ItchRoundTrip, OrderCancel) {
    itch::OrderCancel in{ make_hdr(3, 555ULL), 42ULL, 17 };
    std::uint8_t buf[23];
    const std::size_t n = itch::encode_order_cancel(buf, in);
    ASSERT_EQ(n, itch::message_length('X'));
    EXPECT_EQ(buf[0], 'X');

    const auto out = itch::decode_order_cancel(buf + 1);
    EXPECT_EQ(out.order_ref, 42ULL);
    EXPECT_EQ(out.canceled_shares, 17u);
}

TEST(ItchRoundTrip, OrderDelete) {
    itch::OrderDelete in{ make_hdr(3, 555ULL), 42ULL };
    std::uint8_t buf[19];
    const std::size_t n = itch::encode_order_delete(buf, in);
    ASSERT_EQ(n, itch::message_length('D'));
    EXPECT_EQ(buf[0], 'D');

    const auto out = itch::decode_order_delete(buf + 1);
    EXPECT_EQ(out.order_ref, 42ULL);
}

TEST(ItchRoundTrip, OrderReplace) {
    itch::OrderReplace in{
        make_hdr(3, 555ULL), 42ULL, 43ULL, 250u, 1'010'000u };
    std::uint8_t buf[35];
    const std::size_t n = itch::encode_order_replace(buf, in);
    ASSERT_EQ(n, itch::message_length('U'));
    EXPECT_EQ(buf[0], 'U');

    const auto out = itch::decode_order_replace(buf + 1);
    EXPECT_EQ(out.orig_order_ref, 42ULL);
    EXPECT_EQ(out.new_order_ref, 43ULL);
    EXPECT_EQ(out.new_shares, 250u);
    EXPECT_EQ(out.new_price_x10000, 1'010'000u);
}

TEST(ItchRoundTrip, TradeNonCross) {
    itch::TradeNonCross in{};
    in.hdr          = make_hdr(5, 999ULL);
    in.order_ref    = 0;  // hidden trade
    in.side         = 'B';
    in.shares       = 50;
    std::memcpy(in.stock, "ABC     ", 8);
    in.price_x10000 = 9'999'900u;
    in.match_number = 88ULL;

    std::uint8_t buf[44];
    const std::size_t n = itch::encode_trade_non_cross(buf, in);
    ASSERT_EQ(n, itch::message_length('P'));
    EXPECT_EQ(buf[0], 'P');

    const auto out = itch::decode_trade_non_cross(buf + 1);
    EXPECT_EQ(out.side, 'B');
    EXPECT_EQ(out.shares, 50u);
    EXPECT_EQ(std::memcmp(out.stock, "ABC     ", 8), 0);
    EXPECT_EQ(out.price_x10000, 9'999'900u);
    EXPECT_EQ(out.match_number, 88ULL);
}

// ---- Length-prefixed tape walker ------------------------------------

namespace {

// Write a single message with its 2-byte big-endian length prefix.
template <typename EncodeFn>
std::size_t write_framed(std::vector<std::uint8_t>& tape, std::size_t body_len,
                         EncodeFn encode) {
    const std::size_t off = tape.size();
    tape.resize(off + 2 + body_len);
    itch::write_be16(tape.data() + off, static_cast<std::uint16_t>(body_len));
    const std::size_t written = encode(tape.data() + off + 2);
    EXPECT_EQ(written, body_len);
    return 2 + body_len;
}

}  // namespace

TEST(ItchTape, WalksMixedSequence) {
    std::vector<std::uint8_t> tape;
    tape.reserve(256);

    // R (Stock Directory): introduces stock_locate 7 ↔ "TEST    ".
    write_framed(tape, itch::message_length('R'), [](std::uint8_t* p) {
        itch::StockDirectory m{};
        m.hdr = { 7, 0, 100ULL };
        std::memcpy(m.stock, "TEST    ", 8);
        return itch::encode_stock_directory(p, m);
    });
    // A: add order
    write_framed(tape, itch::message_length('A'), [](std::uint8_t* p) {
        itch::AddOrder m{};
        m.hdr = { 7, 0, 200ULL };
        m.order_ref = 1;
        m.side = 'B';
        m.shares = 100;
        std::memcpy(m.stock, "TEST    ", 8);
        m.price_x10000 = 1'000'000;
        return itch::encode_add_order(p, m);
    });
    // E: execute
    write_framed(tape, itch::message_length('E'), [](std::uint8_t* p) {
        itch::OrderExecuted m{};
        m.hdr = { 7, 0, 300ULL };
        m.order_ref = 1;
        m.executed_shares = 50;
        m.match_number = 1000;
        return itch::encode_order_executed(p, m);
    });
    // D: delete
    write_framed(tape, itch::message_length('D'), [](std::uint8_t* p) {
        itch::OrderDelete m{};
        m.hdr = { 7, 0, 400ULL };
        m.order_ref = 1;
        return itch::encode_order_delete(p, m);
    });

    std::size_t cursor = 0;
    std::vector<char> seen;
    seen.reserve(4);
    while (cursor < tape.size()) {
        itch::TapeFrame frame{};
        std::size_t consumed = 0;
        ASSERT_TRUE(itch::next_frame(tape.data() + cursor,
                                     tape.size() - cursor,
                                     &frame, &consumed));
        seen.push_back(frame.type);
        cursor += consumed;
    }
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0], 'R');
    EXPECT_EQ(seen[1], 'A');
    EXPECT_EQ(seen[2], 'E');
    EXPECT_EQ(seen[3], 'D');
    EXPECT_EQ(cursor, tape.size());
}

TEST(ItchTape, RejectsTruncatedPrefix) {
    std::uint8_t buf[1] = { 0x12 };
    itch::TapeFrame frame{};
    std::size_t consumed = 0;
    EXPECT_FALSE(itch::next_frame(buf, 1, &frame, &consumed));
}

TEST(ItchTape, RejectsTruncatedBody) {
    std::uint8_t buf[5];
    // Claims a 36-byte 'A' message but the buffer is only 5 bytes.
    itch::write_be16(buf, 36);
    buf[2] = 'A';
    itch::TapeFrame frame{};
    std::size_t consumed = 0;
    EXPECT_FALSE(itch::next_frame(buf, 5, &frame, &consumed));
}

TEST(ItchTape, RejectsLengthMismatch) {
    // Length prefix claims 99 bytes but the type is 'A' which the table
    // says is 36. The walker rejects the frame.
    std::uint8_t buf[200] = {};
    itch::write_be16(buf, 99);
    buf[2] = 'A';
    itch::TapeFrame frame{};
    std::size_t consumed = 0;
    EXPECT_FALSE(itch::next_frame(buf, sizeof(buf), &frame, &consumed));
}

TEST(ItchTape, FastSkipUnknownType) {
    // An 'X' message followed by a deliberately unknown type the walker
    // does not recognise. The walker reads the prefix length blindly
    // for unknown types (expected==0 means "trust the prefix"), so we
    // can step over the message and reach a known one after it.
    std::vector<std::uint8_t> tape;
    write_framed(tape, itch::message_length('X'), [](std::uint8_t* p) {
        itch::OrderCancel m{ {1, 0, 0ULL}, 9ULL, 5 };
        return itch::encode_order_cancel(p, m);
    });
    // Unknown type 'z' with a 10-byte body; the walker should accept it
    // (expected length == 0 ⇒ trust the prefix) and step past.
    const std::size_t unk_off = tape.size();
    tape.resize(unk_off + 2 + 10);
    itch::write_be16(tape.data() + unk_off, 10);
    tape[unk_off + 2] = 'z';
    // Followed by a 'D' the walker should successfully recognise.
    write_framed(tape, itch::message_length('D'), [](std::uint8_t* p) {
        itch::OrderDelete m{ {1, 0, 0ULL}, 9ULL };
        return itch::encode_order_delete(p, m);
    });

    std::size_t cursor = 0;
    std::vector<char> seen;
    while (cursor < tape.size()) {
        itch::TapeFrame frame{};
        std::size_t consumed = 0;
        ASSERT_TRUE(itch::next_frame(tape.data() + cursor,
                                     tape.size() - cursor,
                                     &frame, &consumed));
        seen.push_back(frame.type);
        cursor += consumed;
    }
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], 'X');
    EXPECT_EQ(seen[1], 'z');
    EXPECT_EQ(seen[2], 'D');
}
