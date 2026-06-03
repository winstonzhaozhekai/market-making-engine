#ifndef ROLLING_ESTIMATORS_H
#define ROLLING_ESTIMATORS_H

#include "../MarketDataEvent.h"
#include "RingBuffer.h"

#include <cmath>
#include <cstddef>
#include <span>

// Sliding-window sample standard deviation of log-style returns
// (mid_t / mid_{t-1} - 1), computed with West's algorithm for online
// add/remove. sigma() is O(1) per call; the per-tick add/remove pair
// is six FLOPs. Numerically stable across long runs in the same window
// that a naive sum / sum_of_squares pair would drift in.
class RollingVolatility {
public:
    explicit RollingVolatility(std::size_t window = 100)
        : returns_(window > 0 ? window : 1), window_(window) {}

    void on_mid(double mid) {
        if (has_prev_ && prev_mid_ > 0.0) {
            const double ret = (mid - prev_mid_) / prev_mid_;
            if (returns_.size() == window_) {
                const double old = returns_.front();
                returns_.pop_front();
                remove_sample(old);
            }
            returns_.push_back(ret);
            add_sample(ret);
        }
        prev_mid_ = mid;
        has_prev_ = true;
    }

    double sigma() const {
        if (n_ < 2) return 0.0;
        return std::sqrt(m2_ / static_cast<double>(n_ - 1));
    }

    std::size_t count() const { return n_; }

private:
    void add_sample(double x) {
        ++n_;
        const double delta = x - mean_;
        mean_ += delta / static_cast<double>(n_);
        m2_   += delta * (x - mean_);
    }

    void remove_sample(double x) {
        if (n_ == 1) {
            n_ = 0; mean_ = 0.0; m2_ = 0.0;
            return;
        }
        const double delta = x - mean_;
        --n_;
        mean_ -= delta / static_cast<double>(n_);
        m2_   -= delta * (x - mean_);
        if (m2_ < 0.0) m2_ = 0.0;  // clamp FP underflow
    }

    RingBuffer<double> returns_;
    std::size_t        window_;
    double             prev_mid_  = 0.0;
    bool               has_prev_  = false;
    std::size_t        n_         = 0;
    double             mean_      = 0.0;
    double             m2_        = 0.0;
};

// Sliding-window normalized order-flow imbalance: net signed volume
// divided by gross signed volume. O(1) per trade via running net_sum_
// and abs_sum_ maintained alongside the ring.
class RollingOFI {
public:
    explicit RollingOFI(std::size_t window = 50)
        : signed_volumes_(window > 0 ? window : 1), window_(window) {}

    void on_trades(std::span<const Trade> trades) {
        for (const auto& t : trades) {
            const double v = (t.aggressor_side == Side::BUY)
                ?  static_cast<double>(t.size)
                : -static_cast<double>(t.size);
            if (signed_volumes_.size() == window_) {
                const double old = signed_volumes_.front();
                signed_volumes_.pop_front();
                net_sum_ -= old;
                abs_sum_ -= std::abs(old);
            }
            signed_volumes_.push_back(v);
            net_sum_ += v;
            abs_sum_ += std::abs(v);
        }
    }

    double normalized_ofi() const {
        if (abs_sum_ == 0.0) return 0.0;
        return net_sum_ / abs_sum_;
    }

    std::size_t count() const { return signed_volumes_.size(); }

private:
    RingBuffer<double> signed_volumes_;
    std::size_t        window_;
    double             net_sum_ = 0.0;
    double             abs_sum_ = 0.0;
};

#endif // ROLLING_ESTIMATORS_H
