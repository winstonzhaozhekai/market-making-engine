#ifndef SPSC_RING_H
#define SPSC_RING_H

// Single-producer / single-consumer byte ring buffer. Cache-line-padded
// head/tail to avoid false sharing between producer and consumer cores.
// Power-of-two capacity so wrap-around uses a single AND. Drop-on-full
// (HFT-canonical): the producer never blocks; if a record won't fit it
// returns false and bumps a drop counter, never touching the consumer.
//
// Memory ordering: producer publishes head_ with release after writing the
// payload; consumer acquires head_ before reading the payload. Consumer
// publishes tail_ with release after reading; producer acquires tail_
// before writing. Each side has its own cached copy of the other cursor
// updated only when the local fast path would otherwise indicate full /
// empty — keeps both sides from hammering the foreign cache line.
//
// Byte-oriented (not record-typed) so the same ring backs both the
// engine-event logger and the market-data logger.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>

namespace mme_log {

#ifdef __cpp_lib_hardware_interference_size
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;
#endif

class SpscRing {
public:
    explicit SpscRing(std::size_t capacity_bytes) {
        const bool power_of_two =
            capacity_bytes != 0 && (capacity_bytes & (capacity_bytes - 1)) == 0;
        if (capacity_bytes < 64 || !power_of_two) {
            throw std::invalid_argument(
                "SpscRing capacity must be a power of two and >= 64");
        }
        capacity_ = capacity_bytes;
        mask_ = capacity_bytes - 1;
        buf_.assign(capacity_bytes, 0);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const { return capacity_; }
    uint64_t drops() const { return drops_.load(std::memory_order_relaxed); }

    // Producer side. Attempts to reserve `n` contiguous bytes (handles
    // wrap-around as two segments internally). Returns false on full
    // (drop counter incremented). On success, writes `data[0..n)` into
    // the ring and publishes the new head_ with release.
    bool try_push(const char* data, std::size_t n) {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        // Fast path: use the cached tail. Refresh only if it looks full.
        uint64_t tail_cached = cached_tail_;
        if (head - tail_cached + n > capacity_) {
            tail_cached = tail_.load(std::memory_order_acquire);
            cached_tail_ = tail_cached;
            if (head - tail_cached + n > capacity_) {
                drops_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        const std::size_t offset = head & mask_;
        const std::size_t first = std::min(n, capacity_ - offset);
        std::memcpy(buf_.data() + offset, data, first);
        if (first < n) {
            std::memcpy(buf_.data(), data + first, n - first);
        }
        head_.store(head + n, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns the number of bytes currently readable
    // (between cached or refreshed head_ and the local tail_).
    std::size_t readable() const {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(head - tail);
    }

    // Consumer side. Copies up to `max_n` bytes into `out`; returns how many
    // were copied. Advances tail_ with release.
    std::size_t pop_some(char* out, std::size_t max_n) {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t avail = static_cast<std::size_t>(head - tail);
        if (avail == 0) {
            return 0;
        }
        const std::size_t n = std::min(max_n, avail);
        const std::size_t offset = tail & mask_;
        const std::size_t first = std::min(n, capacity_ - offset);
        std::memcpy(out, buf_.data() + offset, first);
        if (first < n) {
            std::memcpy(out + first, buf_.data(), n - first);
        }
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

private:
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::vector<char> buf_;

    // Each cursor on its own cache line. Cached counterparts live next to
    // the cursor that reads them, never on the foreign side's hot line.
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};
    uint64_t cached_tail_ = 0;  // producer-local; touched only by producer
    [[maybe_unused]] char pad1_[kCacheLine - sizeof(std::atomic<uint64_t>) - sizeof(uint64_t)]{};

    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};
    [[maybe_unused]] char pad2_[kCacheLine - sizeof(std::atomic<uint64_t>)]{};

    alignas(kCacheLine) std::atomic<uint64_t> drops_{0};
};

}  // namespace mme_log

#endif  // SPSC_RING_H
