// Asserts that the steady-state MarketMaker hot path (on_market_data
// → strategy → engine submit/amend/cancel → accounting → risk) makes
// zero heap allocations. We override global operator new/delete to bump
// a counter, gated by an RAII guard so that gtest's own allocations and
// the warmup phase don't pollute the measurement.
//
// What this catches: any std::vector::push_back that hits capacity, any
// std::unordered_map bucket grow, any unintended new T(...), any
// std::function bound to a local lambda, etc. Doesn't catch alloca/mmap
// or vendor allocators that bypass operator new — fine for our use,
// where every container is libc++/libstdc++ stock.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "MarketDataEvent.h"
#include "MarketMaker.h"
#include "MarketSimulator.h"
#include "Order.h"
#include "include/Instrument.h"
#include "include/Logger.h"
#include "include/RiskManager.h"
#include "include/SimulationConfig.h"
#include "include/Strategy.h"

namespace {

// Single-threaded counter. active_ gates counting so gtest framework
// allocations and warmup-phase allocations don't pollute the measurement.
struct AllocStats {
    std::size_t count  = 0;
    bool        active = false;
};

AllocStats& stats() {
    static AllocStats s;
    return s;
}

struct ScopedAllocationCounter {
    ScopedAllocationCounter()  { stats().count = 0; stats().active = true; }
    ~ScopedAllocationCounter() { stats().active = false; }
    std::size_t count() const  { return stats().count; }
};

// Strategy that always emits the same quote → MM should converge to
// no-op (same px, same size) after the first tick.
class FixedQuoteStrategy : public Strategy {
public:
    FixedQuoteStrategy(Ticks bp, Ticks ap, int bs, int as)
        : decision_{} {
        decision_.bid_price = bp; decision_.ask_price = ap;
        decision_.bid_size  = bs; decision_.ask_size  = as;
        decision_.should_quote = true;
    }
    QuoteDecision compute_quotes(const StrategySnapshot&) override { return decision_; }
    const char* name() const override { return "fixed"; }
private:
    QuoteDecision decision_;
};

std::chrono::system_clock::time_point at(int64_t ns) {
    using sc = std::chrono::system_clock;
    return sc::time_point{
        std::chrono::duration_cast<sc::duration>(std::chrono::nanoseconds{ns})};
}

MarketDataEvent make_md(Ticks bid_px, Ticks ask_px, int64_t seq, int64_t ts_ns) {
    MarketDataEvent md;
    md.instrument = "TEST";
    md.best_bid_price = bid_px;
    md.best_ask_price = ask_px;
    md.best_bid_size = 100;
    md.best_ask_size = 100;
    md.bid_levels.emplace_back(bid_px, 100, 0ULL, at(ts_ns));
    md.ask_levels.emplace_back(ask_px, 100, 0ULL, at(ts_ns));
    md.timestamp = at(ts_ns);
    md.sequence_number = seq;
    return md;
}

TEST(HotPathAllocations, steady_state_emits_zero_allocations) {
    SimulationConfig cfg;
    cfg.seed = 1;
    cfg.iterations = 0;
    cfg.latency_ms = 0;
    cfg.quiet = true;
    MarketSimulator sim(cfg);
    Instrument ins = sim.instrument_meta();
    Ticks bp = ins.to_ticks(99.95);
    Ticks ap = ins.to_ticks(100.05);

    MarketMaker mm(ins, RiskConfig{},
                   std::make_unique<FixedQuoteStrategy>(bp, ap, 5, 5),
                   std::make_unique<NullLogger>());

    // Pre-build a fixed batch of MarketDataEvents so the loop body does
    // not allocate via event construction. (Simulator-generated events
    // would also allocate inside the loop; M9 LOB rewrite is the right
    // place for that — out of M5 scope.)
    constexpr int kWarmup    = 100;
    constexpr int kMeasured  = 1000;
    std::vector<MarketDataEvent> events;
    events.reserve(kWarmup + kMeasured);
    for (int i = 0; i < kWarmup + kMeasured; ++i) {
        events.push_back(make_md(bp, ap, i + 1, static_cast<int64_t>(i + 1) * 1'000'000));
    }

    // Warmup: first tick allocates the slab chunk and the resting orders;
    // subsequent ticks should converge to the unchanged-quote no-op path.
    for (int i = 0; i < kWarmup; ++i) {
        mm.on_market_data(events[i], sim);
    }

    {
        ScopedAllocationCounter g;
        for (int i = kWarmup; i < kWarmup + kMeasured; ++i) {
            mm.on_market_data(events[i], sim);
        }
        EXPECT_EQ(g.count(), 0u)
            << "Expected zero heap allocations across " << kMeasured
            << " steady-state ticks, observed " << g.count();
    }
}

} // namespace

// ---- Global operator new/delete interposition ------------------------------
// These replace the stock global allocators across the whole test binary.
// We gate counting via stats().active so this doesn't fight gtest.

void* operator new(std::size_t n) {
    if (stats().active) ++stats().count;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    if (stats().active) ++stats().count;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept                  { std::free(p); }
void operator delete[](void* p) noexcept                { std::free(p); }
void operator delete(void* p, std::size_t) noexcept     { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept   { std::free(p); }
