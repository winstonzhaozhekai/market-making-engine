#ifndef ROLLING_ESTIMATORS_H
#define ROLLING_ESTIMATORS_H

#include "../MarketDataEvent.h"
#include <deque>
#include <cmath>
#include <vector>

class RollingVolatility {
public:
    explicit RollingVolatility(size_t window = 100) : window_(window) {}

    void on_mid(double mid) {
        if (!mids_.empty()) {
            double prev = mids_.back();
            if (prev > 0.0) {
                double ret = (mid - prev) / prev;
                returns_.push_back(ret);
                if (returns_.size() > window_) {
                    returns_.pop_front();
                }
            }
        }
        mids_.push_back(mid);
        if (mids_.size() > window_ + 1) {
            mids_.pop_front();
        }
    }

    double sigma() const {
        if (returns_.size() < 2) return 0.0;
        double sum = 0.0;
        for (double r : returns_) sum += r;
        double mean = sum / static_cast<double>(returns_.size());
        double sq_sum = 0.0;
        for (double r : returns_) {
            double diff = r - mean;
            sq_sum += diff * diff;
        }
        return std::sqrt(sq_sum / static_cast<double>(returns_.size() - 1));
    }

    size_t count() const { return returns_.size(); }

private:
    size_t window_;
    std::deque<double> mids_;
    std::deque<double> returns_;
};

class RollingOFI {
public:
    explicit RollingOFI(size_t window = 50) : window_(window) {}

    void on_trades(const std::vector<Trade>& trades) {
        for (const auto& t : trades) {
            double signed_vol = (t.aggressor_side == "BUY") ?
                static_cast<double>(t.size) : -static_cast<double>(t.size);
            signed_volumes_.push_back(signed_vol);
            if (signed_volumes_.size() > window_) {
                signed_volumes_.pop_front();
            }
        }
    }

    double normalized_ofi() const {
        if (signed_volumes_.empty()) return 0.0;
        double net = 0.0;
        double total = 0.0;
        for (double v : signed_volumes_) {
            net += v;
            total += std::abs(v);
        }
        if (total == 0.0) return 0.0;
        return net / total;
    }

    size_t count() const { return signed_volumes_.size(); }

private:
    size_t window_;
    std::deque<double> signed_volumes_;
};

#endif // ROLLING_ESTIMATORS_H
