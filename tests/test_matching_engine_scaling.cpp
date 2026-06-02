#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>
#include "MatchingEngine.h"
#include "Order.h"

namespace {

auto ts0 = std::chrono::system_clock::time_point(std::chrono::milliseconds(1));

// Returns median nanoseconds-per-cancel across N orders inserted at
// distinct prices (worst case for cancel — every cancel also erases its
// price level from the map, paying the O(log num_levels) cost).
double median_cancel_ns(std::size_t N) {
    MatchingEngine engine;
    std::vector<uint64_t> ids;
    ids.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        // Distinct price per order: tick i+1.
        Ticks px = static_cast<Ticks>(i + 1);
        uint64_t id = i + 1;
        auto r = engine.add_order(Order(id, Side::BUY, px, 1, ts0),
                                  OrderType::POST_ONLY);
        // POST_ONLY rests cleanly at distinct prices below any ask
        // (no asks exist), so all should ACK.
        if (r.status != OrderStatus::ACKNOWLEDGED) {
            return -1.0;
        }
        ids.push_back(id);
    }
    // Cancel in randomized order so we hit median-depth map nodes, not
    // best-of-book each time.
    std::mt19937 rng(0xC0FFEE);
    std::shuffle(ids.begin(), ids.end(), rng);

    std::vector<int64_t> samples;
    samples.reserve(N);
    for (uint64_t id : ids) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = engine.cancel_order(id);
        auto t1 = std::chrono::steady_clock::now();
        if (!ok) return -1.0;
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    std::nth_element(samples.begin(),
                     samples.begin() + samples.size() / 2,
                     samples.end());
    return static_cast<double>(samples[samples.size() / 2]);
}

// O(1)-ish cancel: per-cancel cost should be near-constant in N. We
// accept an upper bound of 5x ratio between N=100k and N=1k — generous
// enough to absorb cache effects and OS scheduling noise, tight enough
// to fail clearly if cancel were O(N) (which would produce a ~100x ratio).
//
// This is a property-style scaling check, not a published-ns benchmark.
// Real microbench numbers land in M11 with core pinning and frequency
// lock.
TEST(MatchingEngineScaling, cancel_is_o1_ish) {
    double t1k   = median_cancel_ns(1'000);
    double t10k  = median_cancel_ns(10'000);
    double t100k = median_cancel_ns(100'000);

    ASSERT_GT(t1k,   0.0);
    ASSERT_GT(t10k,  0.0);
    ASSERT_GT(t100k, 0.0);

    std::cerr << "[scaling] median cancel ns: "
              << "N=1k:"   << t1k   << "  "
              << "N=10k:"  << t10k  << "  "
              << "N=100k:" << t100k << "\n";

    // Permissive upper bound: linear cancel would be ~100x at N=100k vs
    // N=1k. Hash + intrusive unlink + O(log levels) erase should sit
    // well under 5x.
    EXPECT_LT(t100k / t1k, 5.0)
        << "cancel scaling looks worse than O(1)-ish: "
        << "t1k=" << t1k << " t100k=" << t100k;
}

}  // namespace
