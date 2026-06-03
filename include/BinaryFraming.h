#ifndef BINARY_FRAMING_H
#define BINARY_FRAMING_H

// Shared on-disk framing for the M6 binary logs (engine-event SpscLogger and
// MarketDataEvent SpscMdLogger). Both produce files with the same 16-byte
// header followed by length-prefixed, type-tagged records. Little-endian on
// the wire — readers verify via the endian sentinel and refuse mismatches.
//
// File layout:
//   [Header: 16 bytes]
//   [Record][Record]...[Record]
//
// Header:
//   magic       u32   'MMEL' (0x4C454D4D LE)
//   version     u16   = 1
//   endian_sig  u16   = 0xCAFE   (0xFECA → wrong endianness)
//   reserved    u64   = 0        (future: stream id, drops counter, etc.)
//
// Record:
//   payload_len u32   (excludes this u32 and the type byte; counts payload only)
//   type        u8    (RecordType)
//   payload     bytes (type-dispatched)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mme_log {

constexpr uint32_t kMagic       = 0x4C454D4D;  // 'MMEL' little-endian
constexpr uint16_t kVersion     = 1;
constexpr uint16_t kEndianSig   = 0xCAFE;
constexpr std::size_t kHeaderBytes = 16;

enum class RecordType : uint8_t {
    MD_EVENT      = 0x01,
    FILL          = 0x10,
    SEQ_GAP       = 0x11,
    EMPTY_BOOK    = 0x12,
    AMEND_REJECT  = 0x13,
};

struct FileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t endian_sig;
    uint64_t reserved;
};
static_assert(sizeof(FileHeader) == kHeaderBytes,
              "FileHeader must pack to 16 bytes");

inline FileHeader make_header() {
    return FileHeader{kMagic, kVersion, kEndianSig, 0};
}

// Trivially-copyable scalar IO. Memcpy avoids strict-aliasing UB and works
// for any T that is bit-copyable (which is all we serialize).
template <typename T>
inline void put(char*& cursor, const T& v) {
    static_assert(std::is_trivially_copyable<T>::value, "put requires trivially-copyable T");
    std::memcpy(cursor, &v, sizeof(T));
    cursor += sizeof(T);
}

template <typename T>
inline T take(const char*& cursor) {
    static_assert(std::is_trivially_copyable<T>::value, "take requires trivially-copyable T");
    T v;
    std::memcpy(&v, cursor, sizeof(T));
    cursor += sizeof(T);
    return v;
}

// Record framing helpers. payload_len is the size of payload bytes only
// (excludes payload_len itself and the type byte). Total bytes on the wire
// for a record = sizeof(uint32_t) + 1 + payload_len.
inline constexpr std::size_t framed_size(std::size_t payload_len) {
    return sizeof(uint32_t) + 1u + payload_len;
}

// Write a record header (length + type) into the buffer. Returns a pointer
// to the payload region. Caller must have allocated at least
// framed_size(payload_len) bytes starting at `out`.
inline char* write_record_header(char* out, RecordType type, uint32_t payload_len) {
    std::memcpy(out, &payload_len, sizeof(payload_len));
    out += sizeof(payload_len);
    *out = static_cast<char>(type);
    out += 1;
    return out;
}

}  // namespace mme_log

#endif  // BINARY_FRAMING_H
