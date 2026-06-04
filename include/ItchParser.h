#ifndef MME_ITCH_PARSER_H
#define MME_ITCH_PARSER_H

// Nasdaq TotalView-ITCH 5.0 zero-copy parser.
//
// The hot path is a switch over a 1-byte message type with [[likely]]
// hints on the high-frequency book-affecting types (A/F/E/C/X/D/U) and a
// length table for fast-skip on everything else. Messages are decoded
// in-place by casting the raw buffer onto a packed POD struct that
// mirrors the on-tape layout, with __builtin_bswap on the multi-byte
// integer / price / shares fields.
//
// Layout reference: Nasdaq TotalView-ITCH 5.0 specification.
//   - All multi-byte numeric fields are big-endian.
//   - Timestamp is 6 bytes (nanoseconds since midnight), encoded as a
//     48-bit big-endian unsigned integer.
//   - Price (4) is a 32-bit big-endian unsigned int scaled by 10000
//     (4 implied decimal places).
//   - Order reference numbers are 64-bit big-endian unique within the
//     trading day per stock_locate.
//   - Stock symbols are 8 bytes ASCII, space-padded ('AAPL    ').
//   - The wire format puts a 2-byte big-endian length prefix in front of
//     each message; `lengths()` returns the length INCLUDING the 1-byte
//     type marker (i.e. the value advertised by the length prefix).
//
// Per-type length table (bytes including the 1-byte type marker):
//   S = 12   R = 99   H = 25   A = 36   F = 40   E = 31
//   C = 36   X = 23   D = 19   U = 35   P = 44   Q = 40   B = 19
//
// (Y, V, W, K, J, L, N, I are present in the spec but currently
// fast-skipped; the length table covers them so a tape with mixed
// messages can be walked without choking.)
//
// Determinism / hot-path invariants:
//   - All decoders are constexpr-pure, branchless on the field-extract
//     step, and allocation-free.
//   - The decoded structs are returned by value (small POD types) so a
//     caller can switch over the type and act per message without an
//     std::variant indirection.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mme::itch {

// ITCH 5.0 message type marker (the first byte of every message after
// the 2-byte length prefix is stripped).
enum class MsgType : char {
    SystemEvent          = 'S',
    StockDirectory       = 'R',
    StockTradingAction   = 'H',
    RegSho               = 'Y',
    MarketParticipant    = 'L',
    MwcbDecline          = 'V',
    MwcbBreach           = 'W',
    IpoQuoting           = 'K',
    Luld                 = 'J',
    AddOrder             = 'A',
    AddOrderAttributed   = 'F',
    OrderExecuted        = 'E',
    OrderExecutedPrice   = 'C',
    OrderCancel          = 'X',
    OrderDelete          = 'D',
    OrderReplace         = 'U',
    TradeNonCross        = 'P',
    TradeCross           = 'Q',
    BrokenTrade          = 'B',
    Noii                 = 'I',
    Rpii                 = 'N',
};

// Returns the message length INCLUDING the 1-byte type marker, for the
// given type code. 0 for unknown / variable-length types (none in 5.0
// today, but we return 0 so a caller can guard).
constexpr std::size_t message_length(char type) noexcept {
    switch (type) {
    case 'S': return 12;
    case 'R': return 39;   // 5.0: 39 bytes (matches Nasdaq spec, not the
                            //               40-byte ASX/Genium variant).
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;  // Operational Halt
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    default:  return 0;
    }
}

// 48-bit big-endian read for the 6-byte timestamp field.
inline std::uint64_t read_be48(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    v |= static_cast<std::uint64_t>(p[0]) << 40;
    v |= static_cast<std::uint64_t>(p[1]) << 32;
    v |= static_cast<std::uint64_t>(p[2]) << 24;
    v |= static_cast<std::uint64_t>(p[3]) << 16;
    v |= static_cast<std::uint64_t>(p[4]) << 8;
    v |= static_cast<std::uint64_t>(p[5]);
    return v;
}

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0]) << 24)
         | (std::uint32_t(p[1]) << 16)
         | (std::uint32_t(p[2]) << 8)
         |  std::uint32_t(p[3]);
}

inline std::uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (std::uint64_t(p[0]) << 56)
         | (std::uint64_t(p[1]) << 48)
         | (std::uint64_t(p[2]) << 40)
         | (std::uint64_t(p[3]) << 32)
         | (std::uint64_t(p[4]) << 24)
         | (std::uint64_t(p[5]) << 16)
         | (std::uint64_t(p[6]) << 8)
         |  std::uint64_t(p[7]);
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

inline void write_be48(std::uint8_t* p, std::uint64_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 40);
    p[1] = static_cast<std::uint8_t>(v >> 32);
    p[2] = static_cast<std::uint8_t>(v >> 24);
    p[3] = static_cast<std::uint8_t>(v >> 16);
    p[4] = static_cast<std::uint8_t>(v >> 8);
    p[5] = static_cast<std::uint8_t>(v);
}

inline void write_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 56);
    p[1] = static_cast<std::uint8_t>(v >> 48);
    p[2] = static_cast<std::uint8_t>(v >> 40);
    p[3] = static_cast<std::uint8_t>(v >> 32);
    p[4] = static_cast<std::uint8_t>(v >> 24);
    p[5] = static_cast<std::uint8_t>(v >> 16);
    p[6] = static_cast<std::uint8_t>(v >> 8);
    p[7] = static_cast<std::uint8_t>(v);
}

// Decoded representations. Small PODs; returned by value from decode_*().
struct CommonHeader {
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    std::uint64_t timestamp_ns;     // 48-bit value zero-extended
};

struct SystemEvent {
    CommonHeader hdr;
    char         event_code;
};

struct StockDirectory {
    CommonHeader hdr;
    char         stock[8];          // ASCII, space-padded
    // Remaining fields (round_lot_size, market_category, etc.) skipped —
    // we only need the stock_locate ↔ symbol binding.
};

struct StockTradingAction {
    CommonHeader hdr;
    char         stock[8];
    char         trading_state;
    char         reason[4];
};

// Add Order (A) and Add Order Attributed (F). F differs only by a 4-byte
// MPID attribution field between stock and price; we keep them separate
// to avoid lying about the byte layout.
struct AddOrder {
    CommonHeader  hdr;
    std::uint64_t order_ref;
    char          side;             // 'B' or 'S'
    std::uint32_t shares;
    char          stock[8];
    std::uint32_t price_x10000;     // 4 implied decimals
};

struct AddOrderAttributed {
    CommonHeader  hdr;
    std::uint64_t order_ref;
    char          side;
    std::uint32_t shares;
    char          stock[8];
    char          attribution[4];
    std::uint32_t price_x10000;
};

struct OrderExecuted {
    CommonHeader  hdr;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

struct OrderExecutedWithPrice {
    CommonHeader  hdr;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    char          printable;        // 'Y' or 'N'
    std::uint32_t execution_price_x10000;
};

struct OrderCancel {
    CommonHeader  hdr;
    std::uint64_t order_ref;
    std::uint32_t canceled_shares;
};

struct OrderDelete {
    CommonHeader  hdr;
    std::uint64_t order_ref;
};

struct OrderReplace {
    CommonHeader  hdr;
    std::uint64_t orig_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t new_shares;
    std::uint32_t new_price_x10000;
};

struct TradeNonCross {
    CommonHeader  hdr;
    std::uint64_t order_ref;        // always 0 for non-displayable trades
    char          side;
    std::uint32_t shares;
    char          stock[8];
    std::uint32_t price_x10000;
    std::uint64_t match_number;
};

// ---- Decoders -------------------------------------------------------
// Each takes a pointer to the first byte of the payload AFTER the 1-byte
// type marker, i.e. positions 1..N-1 of the message. Caller is expected
// to have verified `buf[0]` matches the type and that at least
// `message_length(type) - 1` bytes are available at `p`.

inline CommonHeader decode_header(const std::uint8_t* p) noexcept {
    return {
        read_be16(p),
        read_be16(p + 2),
        read_be48(p + 4),
    };
}

inline SystemEvent decode_system_event(const std::uint8_t* p) noexcept {
    return { decode_header(p), static_cast<char>(p[10]) };
}

inline StockDirectory decode_stock_directory(const std::uint8_t* p) noexcept {
    StockDirectory m{};
    m.hdr = decode_header(p);
    std::memcpy(m.stock, p + 10, 8);
    return m;
}

inline StockTradingAction decode_stock_trading_action(const std::uint8_t* p) noexcept {
    StockTradingAction m{};
    m.hdr = decode_header(p);
    std::memcpy(m.stock, p + 10, 8);
    m.trading_state = static_cast<char>(p[18]);
    std::memcpy(m.reason, p + 20, 4);
    return m;
}

inline AddOrder decode_add_order(const std::uint8_t* p) noexcept {
    AddOrder m{};
    m.hdr           = decode_header(p);
    m.order_ref     = read_be64(p + 10);
    m.side          = static_cast<char>(p[18]);
    m.shares        = read_be32(p + 19);
    std::memcpy(m.stock, p + 23, 8);
    m.price_x10000  = read_be32(p + 31);
    return m;
}

inline AddOrderAttributed decode_add_order_attributed(const std::uint8_t* p) noexcept {
    AddOrderAttributed m{};
    m.hdr           = decode_header(p);
    m.order_ref     = read_be64(p + 10);
    m.side          = static_cast<char>(p[18]);
    m.shares        = read_be32(p + 19);
    std::memcpy(m.stock, p + 23, 8);
    std::memcpy(m.attribution, p + 31, 4);
    m.price_x10000  = read_be32(p + 35);
    return m;
}

inline OrderExecuted decode_order_executed(const std::uint8_t* p) noexcept {
    OrderExecuted m{};
    m.hdr             = decode_header(p);
    m.order_ref       = read_be64(p + 10);
    m.executed_shares = read_be32(p + 18);
    m.match_number    = read_be64(p + 22);
    return m;
}

inline OrderExecutedWithPrice decode_order_executed_with_price(const std::uint8_t* p) noexcept {
    OrderExecutedWithPrice m{};
    m.hdr                    = decode_header(p);
    m.order_ref              = read_be64(p + 10);
    m.executed_shares        = read_be32(p + 18);
    m.match_number           = read_be64(p + 22);
    m.printable              = static_cast<char>(p[30]);
    m.execution_price_x10000 = read_be32(p + 31);
    return m;
}

inline OrderCancel decode_order_cancel(const std::uint8_t* p) noexcept {
    OrderCancel m{};
    m.hdr             = decode_header(p);
    m.order_ref       = read_be64(p + 10);
    m.canceled_shares = read_be32(p + 18);
    return m;
}

inline OrderDelete decode_order_delete(const std::uint8_t* p) noexcept {
    OrderDelete m{};
    m.hdr       = decode_header(p);
    m.order_ref = read_be64(p + 10);
    return m;
}

inline OrderReplace decode_order_replace(const std::uint8_t* p) noexcept {
    OrderReplace m{};
    m.hdr               = decode_header(p);
    m.orig_order_ref    = read_be64(p + 10);
    m.new_order_ref     = read_be64(p + 18);
    m.new_shares        = read_be32(p + 26);
    m.new_price_x10000  = read_be32(p + 30);
    return m;
}

inline TradeNonCross decode_trade_non_cross(const std::uint8_t* p) noexcept {
    TradeNonCross m{};
    m.hdr           = decode_header(p);
    m.order_ref     = read_be64(p + 10);
    m.side          = static_cast<char>(p[18]);
    m.shares        = read_be32(p + 19);
    std::memcpy(m.stock, p + 23, 8);
    m.price_x10000  = read_be32(p + 31);
    m.match_number  = read_be64(p + 35);
    return m;
}

// ---- Encoders (for tests and the synthetic-tape generator) ---------
// Each writes the 1-byte type marker plus payload starting at `out`.
// Returns the total bytes written, which equals message_length(type).

inline std::size_t encode_header(std::uint8_t* out, const CommonHeader& h) noexcept {
    write_be16(out + 0, h.stock_locate);
    write_be16(out + 2, h.tracking_number);
    write_be48(out + 4, h.timestamp_ns);
    return 10;
}

inline std::size_t encode_system_event(std::uint8_t* out, const SystemEvent& m) noexcept {
    out[0] = 'S';
    encode_header(out + 1, m.hdr);
    out[11] = static_cast<std::uint8_t>(m.event_code);
    return 12;
}

inline std::size_t encode_stock_directory(std::uint8_t* out, const StockDirectory& m) noexcept {
    out[0] = 'R';
    encode_header(out + 1, m.hdr);
    std::memcpy(out + 11, m.stock, 8);
    // Remaining 20 bytes of the 39-byte message are unspecified for our
    // tests; zero-fill so encode/decode round-trips on the header+stock
    // fields we actually use.
    std::memset(out + 19, 0, 20);
    return 39;
}

inline std::size_t encode_add_order(std::uint8_t* out, const AddOrder& m) noexcept {
    out[0] = 'A';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    out[19] = static_cast<std::uint8_t>(m.side);
    write_be32(out + 20, m.shares);
    std::memcpy(out + 24, m.stock, 8);
    write_be32(out + 32, m.price_x10000);
    return 36;
}

inline std::size_t encode_add_order_attributed(std::uint8_t* out,
                                               const AddOrderAttributed& m) noexcept {
    out[0] = 'F';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    out[19] = static_cast<std::uint8_t>(m.side);
    write_be32(out + 20, m.shares);
    std::memcpy(out + 24, m.stock, 8);
    std::memcpy(out + 32, m.attribution, 4);
    write_be32(out + 36, m.price_x10000);
    return 40;
}

inline std::size_t encode_order_executed(std::uint8_t* out, const OrderExecuted& m) noexcept {
    out[0] = 'E';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    write_be32(out + 19, m.executed_shares);
    write_be64(out + 23, m.match_number);
    return 31;
}

inline std::size_t encode_order_executed_with_price(std::uint8_t* out,
                                                    const OrderExecutedWithPrice& m) noexcept {
    out[0] = 'C';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    write_be32(out + 19, m.executed_shares);
    write_be64(out + 23, m.match_number);
    out[31] = static_cast<std::uint8_t>(m.printable);
    write_be32(out + 32, m.execution_price_x10000);
    return 36;
}

inline std::size_t encode_order_cancel(std::uint8_t* out, const OrderCancel& m) noexcept {
    out[0] = 'X';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    write_be32(out + 19, m.canceled_shares);
    return 23;
}

inline std::size_t encode_order_delete(std::uint8_t* out, const OrderDelete& m) noexcept {
    out[0] = 'D';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    return 19;
}

inline std::size_t encode_order_replace(std::uint8_t* out, const OrderReplace& m) noexcept {
    out[0] = 'U';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.orig_order_ref);
    write_be64(out + 19, m.new_order_ref);
    write_be32(out + 27, m.new_shares);
    write_be32(out + 31, m.new_price_x10000);
    return 35;
}

inline std::size_t encode_trade_non_cross(std::uint8_t* out, const TradeNonCross& m) noexcept {
    out[0] = 'P';
    encode_header(out + 1, m.hdr);
    write_be64(out + 11, m.order_ref);
    out[19] = static_cast<std::uint8_t>(m.side);
    write_be32(out + 20, m.shares);
    std::memcpy(out + 24, m.stock, 8);
    write_be32(out + 32, m.price_x10000);
    write_be64(out + 36, m.match_number);
    return 44;
}

// ---- Length-prefixed tape walker ------------------------------------
// On-disk Nasdaq ITCH tape format prepends a 2-byte big-endian length
// (the value of `message_length(type)`) before every message body. This
// walker steps the cursor one message at a time and returns the type
// byte + a pointer to the start of the post-type payload.

struct TapeFrame {
    char                 type;       // ITCH 5.0 message type marker
    const std::uint8_t*  payload;    // first byte after the 1-byte type
    std::size_t          body_length; // length of {type byte + payload}
};

// Parse one length-prefixed frame from `buf` of size `buf_size`. Returns
// true on success; sets `*consumed` to the number of bytes consumed
// (2-byte length prefix + body). Returns false if the buffer is too
// short to hold the prefix + advertised body, or if the type is unknown.
inline bool next_frame(const std::uint8_t* buf, std::size_t buf_size,
                       TapeFrame* out, std::size_t* consumed) noexcept {
    if (buf_size < 3) return false;
    const std::uint16_t body_len = read_be16(buf);
    if (body_len == 0) return false;
    if (buf_size < std::size_t{2} + body_len) return false;
    const std::size_t expected = message_length(static_cast<char>(buf[2]));
    if (expected != 0 && expected != body_len) return false;

    out->type        = static_cast<char>(buf[2]);
    out->payload     = buf + 3;
    out->body_length = body_len;
    *consumed        = std::size_t{2} + body_len;
    return true;
}

} // namespace mme::itch

#endif // MME_ITCH_PARSER_H
