#ifndef MME_HDR_HISTOGRAM_H
#define MME_HDR_HISTOGRAM_H

// Header-only HDR (High Dynamic Range) histogram, a faithful port of the
// core of Gil Tene's HdrHistogram (https://hdrhistogram.org) tuned to the
// engine's latency range. It replaces the nearest-rank percentile math in
// PerformanceModule (ROADMAP tooling issue m7: nearest-rank is lossy at
// p99.9 with small N) with constant-time recording and bounded-error,
// interpolated percentile queries.
//
// Algorithm (unchanged from the reference): the value range is split into
// power-of-two "buckets"; each bucket is subdivided into a fixed number of
// linear "sub-buckets" sized so that any recorded value is stored with at
// least `significant_figures` decimal digits of resolution. Recording is a
// branch-light index computation + a single counter increment; a query
// walks the (small, fixed-length) counts array once. The returned value is
// the highest value equivalent to the bucket the requested rank falls in,
// so two values within one bucket's resolution compare equal — exactly the
// HdrHistogram contract.
//
// This is a single-threaded recorder: the bench harness records on one
// thread and queries after the timed loop. No synchronization.
//
// Tuning: for latency in nanoseconds the engine constructs it as
// HdrHistogram(1, 1'000'000'000, 3) — 1 ns lowest discernible value, 1 s
// ceiling, 3 significant figures. That is ~21k int64 counters (~170 KB),
// allocated once.

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mme {

class HdrHistogram {
public:
    // lowest_discernible_value: smallest value distinguishable from 0 (>= 1).
    // highest_trackable_value:  ceiling (>= 2 * lowest_discernible_value).
    // significant_figures:      decimal digits of resolution, 1..5.
    HdrHistogram(std::int64_t lowest_discernible_value,
                 std::int64_t highest_trackable_value,
                 int significant_figures)
        : lowest_discernible_value_(lowest_discernible_value),
          highest_trackable_value_(highest_trackable_value) {
        if (lowest_discernible_value < 1) {
            throw std::invalid_argument("lowest_discernible_value must be >= 1");
        }
        if (highest_trackable_value < 2 * lowest_discernible_value) {
            throw std::invalid_argument(
                "highest_trackable_value must be >= 2 * lowest_discernible_value");
        }
        if (significant_figures < 1 || significant_figures > 5) {
            throw std::invalid_argument("significant_figures must be in [1, 5]");
        }

        const std::int64_t largest_value_with_single_unit_resolution =
            2 * pow10(significant_figures);
        const int sub_bucket_count_magnitude = static_cast<int>(
            ceil_log2(largest_value_with_single_unit_resolution));
        sub_bucket_half_count_magnitude_ =
            (sub_bucket_count_magnitude > 1 ? sub_bucket_count_magnitude : 1) - 1;

        unit_magnitude_ = static_cast<int>(floor_log2(lowest_discernible_value));

        sub_bucket_count_ =
            static_cast<std::int32_t>(1) << (sub_bucket_half_count_magnitude_ + 1);
        sub_bucket_half_count_ = sub_bucket_count_ / 2;
        sub_bucket_mask_ = (static_cast<std::int64_t>(sub_bucket_count_) - 1)
                           << unit_magnitude_;

        bucket_count_ = buckets_needed_for(highest_trackable_value);
        counts_.assign(counts_array_length(), 0);
    }

    // Record one observation. Values outside [lowest, highest] are clamped
    // into range — a latency recorder must never throw on an outlier spike.
    void record_value(std::int64_t value) {
        if (value < lowest_discernible_value_) value = lowest_discernible_value_;
        if (value > highest_trackable_value_) value = highest_trackable_value_;

        const std::size_t idx = counts_index_for(value);
        ++counts_[idx];
        ++total_count_;
        if (value > max_value_) max_value_ = value;
        if (value < min_value_) min_value_ = value;
    }

    std::int64_t total_count() const { return total_count_; }
    bool empty() const { return total_count_ == 0; }

    std::int64_t min() const { return total_count_ == 0 ? 0 : min_value_; }
    std::int64_t max() const { return total_count_ == 0 ? 0 : max_value_; }

    double mean() const {
        if (total_count_ == 0) return 0.0;
        std::int64_t total = 0;
        long double weighted = 0.0L;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            if (counts_[i] == 0) continue;
            total += counts_[i];
            weighted += static_cast<long double>(counts_[i]) *
                        static_cast<long double>(median_equivalent_value(value_from_index(i)));
        }
        (void)total;
        return static_cast<double>(weighted / static_cast<long double>(total_count_));
    }

    // quantile in [0, 1]. Returns the highest value equivalent to the
    // bucket the requested rank lands in (HdrHistogram convention), so
    // p99.9 is reported with bounded relative error, not snapped to a raw
    // sample as nearest-rank would.
    std::int64_t value_at_quantile(double quantile) const {
        if (total_count_ == 0) return 0;
        if (quantile < 0.0) quantile = 0.0;
        if (quantile > 1.0) quantile = 1.0;

        std::int64_t count_at_quantile =
            static_cast<std::int64_t>(quantile * static_cast<double>(total_count_) + 0.5);
        if (count_at_quantile < 1) count_at_quantile = 1;

        std::int64_t running = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            running += counts_[i];
            if (running >= count_at_quantile) {
                const std::int64_t value = value_from_index(i);
                return quantile == 0.0 ? lowest_equivalent_value(value)
                                       : highest_equivalent_value(value);
            }
        }
        return highest_equivalent_value(max_value_);
    }

    std::int64_t value_at_percentile(double percentile) const {
        return value_at_quantile(percentile / 100.0);
    }

    // Two values are "equivalent" if they fall in the same sub-bucket, i.e.
    // are indistinguishable at the configured resolution. Useful for tests.
    bool values_are_equivalent(std::int64_t a, std::int64_t b) const {
        return lowest_equivalent_value(a) == lowest_equivalent_value(b);
    }

private:
    static std::int64_t pow10(int n) {
        std::int64_t r = 1;
        for (int i = 0; i < n; ++i) r *= 10;
        return r;
    }

    // floor(log2(v)) for v >= 1.
    static int floor_log2(std::int64_t v) {
        int r = 0;
        while (v > 1) {
            v >>= 1;
            ++r;
        }
        return r;
    }

    // ceil(log2(v)) for v >= 1.
    static int ceil_log2(std::int64_t v) {
        int r = 0;
        std::int64_t p = 1;
        while (p < v) {
            p <<= 1;
            ++r;
        }
        return r;
    }

    int buckets_needed_for(std::int64_t value) const {
        std::int64_t smallest_untrackable =
            static_cast<std::int64_t>(sub_bucket_count_) << unit_magnitude_;
        int buckets = 1;
        while (smallest_untrackable <= value) {
            if (smallest_untrackable > (std::numeric_limits<std::int64_t>::max() / 2)) {
                return buckets + 1;
            }
            smallest_untrackable <<= 1;
            ++buckets;
        }
        return buckets;
    }

    std::size_t counts_array_length() const {
        return static_cast<std::size_t>((bucket_count_ + 1) * (sub_bucket_count_ / 2));
    }

    int bucket_index_for(std::int64_t value) const {
        // Number of leading zeros of (value | sub_bucket_mask_), then mapped
        // into the bucket range via the precomputable base.
        const int leading_zero_count_base =
            64 - unit_magnitude_ - (sub_bucket_half_count_magnitude_ + 1);
        return leading_zero_count_base - count_leading_zeros(value | sub_bucket_mask_);
    }

    int sub_bucket_index_for(std::int64_t value, int bucket_index) const {
        return static_cast<int>(
            static_cast<std::uint64_t>(value) >> (bucket_index + unit_magnitude_));
    }

    std::size_t counts_index(int bucket_index, int sub_bucket_index) const {
        const int bucket_base_index = (bucket_index + 1) << sub_bucket_half_count_magnitude_;
        const int offset_in_bucket = sub_bucket_index - sub_bucket_half_count_;
        return static_cast<std::size_t>(bucket_base_index + offset_in_bucket);
    }

    std::size_t counts_index_for(std::int64_t value) const {
        const int bucket_index = bucket_index_for(value);
        const int sub_bucket_index = sub_bucket_index_for(value, bucket_index);
        return counts_index(bucket_index, sub_bucket_index);
    }

    std::int64_t value_from_sub_bucket(int bucket_index, int sub_bucket_index) const {
        return static_cast<std::int64_t>(sub_bucket_index)
               << (bucket_index + unit_magnitude_);
    }

    std::int64_t value_from_index(std::size_t index) const {
        int bucket_index =
            static_cast<int>(index >> sub_bucket_half_count_magnitude_) - 1;
        int sub_bucket_index =
            static_cast<int>(index & (sub_bucket_half_count_ - 1)) + sub_bucket_half_count_;
        if (bucket_index < 0) {
            sub_bucket_index -= sub_bucket_half_count_;
            bucket_index = 0;
        }
        return value_from_sub_bucket(bucket_index, sub_bucket_index);
    }

    std::int64_t size_of_equivalent_value_range(std::int64_t value) const {
        const int bucket_index = bucket_index_for(value);
        const int sub_bucket_index = sub_bucket_index_for(value, bucket_index);
        const int adjusted_bucket =
            (sub_bucket_index >= sub_bucket_count_) ? bucket_index + 1 : bucket_index;
        return static_cast<std::int64_t>(1) << (unit_magnitude_ + adjusted_bucket);
    }

    std::int64_t lowest_equivalent_value(std::int64_t value) const {
        const int bucket_index = bucket_index_for(value);
        const int sub_bucket_index = sub_bucket_index_for(value, bucket_index);
        return value_from_sub_bucket(bucket_index, sub_bucket_index);
    }

    std::int64_t highest_equivalent_value(std::int64_t value) const {
        return lowest_equivalent_value(value) + size_of_equivalent_value_range(value) - 1;
    }

    std::int64_t median_equivalent_value(std::int64_t value) const {
        return lowest_equivalent_value(value) + (size_of_equivalent_value_range(value) >> 1);
    }

    static int count_leading_zeros(std::int64_t v) {
        if (v == 0) return 64;
        return __builtin_clzll(static_cast<unsigned long long>(v));
    }

    std::int64_t lowest_discernible_value_;
    std::int64_t highest_trackable_value_;

    int unit_magnitude_ = 0;
    int sub_bucket_half_count_magnitude_ = 0;
    std::int32_t sub_bucket_count_ = 0;
    std::int32_t sub_bucket_half_count_ = 0;
    std::int64_t sub_bucket_mask_ = 0;
    int bucket_count_ = 0;

    std::vector<std::int64_t> counts_;
    std::int64_t total_count_ = 0;
    std::int64_t min_value_ = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_value_ = 0;
};

}  // namespace mme

#endif  // MME_HDR_HISTOGRAM_H
