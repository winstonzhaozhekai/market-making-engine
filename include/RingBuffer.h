#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <cstddef>
#include <vector>

// Fixed-capacity FIFO ring buffer. Capacity is fixed at construction
// (rounded up to a power of two so head advancement is a single AND),
// backing storage is a single std::vector reserved once in the ctor —
// no allocations on push/pop, no per-node overhead.
//
// Push semantics under overflow: drop the oldest element to make room.
// The risk-manager use case sizes the buffer with a safety margin above
// the rate limit, so overflow indicates the strategy is being rate-
// limited by the risk manager anyway; dropping the oldest is a safe
// degradation under burst.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t min_capacity) {
        capacity_ = round_up_pow2(min_capacity < 2 ? 2 : min_capacity);
        mask_     = capacity_ - 1;
        data_.resize(capacity_);
    }

    void push_back(const T& v) {
        if (size_ == capacity_) {
            head_ = (head_ + 1) & mask_;
            --size_;
        }
        data_[(head_ + size_) & mask_] = v;
        ++size_;
    }

    const T& front() const { return data_[head_]; }

    void pop_front() {
        head_ = (head_ + 1) & mask_;
        --size_;
    }

    std::size_t size()     const { return size_; }
    bool        empty()    const { return size_ == 0; }
    std::size_t capacity() const { return capacity_; }

private:
    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t r = 1;
        while (r < n) r <<= 1;
        return r;
    }

    std::vector<T> data_;
    std::size_t    head_     = 0;
    std::size_t    size_     = 0;
    std::size_t    capacity_ = 0;
    std::size_t    mask_     = 0;
};

#endif // RING_BUFFER_H
