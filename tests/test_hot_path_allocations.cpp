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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "MarketDataEvent.h"
#include "MarketMaker.h"
#include "MarketSimulator.h"
#include "Order.h"
#include "include/Instrument.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/RiskManager.h"
#include "include/SimulationConfig.h"
#include "include/SpscLogger.h"
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
// no-op (same px, same size) after the first tick. `final` so the
// templated MarketMakerT<FixedQuoteStrategy, LoggerT> hot path
// devirtualizes compute_quotes.
class FixedQuoteStrategy final : public Strategy {
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

// Drives the MM hot path through `kMeasured` no-op ticks inside the
// allocation-counter guard. Templated on LoggerT so we can exercise the
// same invariant with NullLogger and with SpscLogger (the M6 closure of
// C3 proper). MarketMakerT instantiated on the concrete final types
// here, matching the M7 devirtualized hot path (no virtual dispatch on
// compute_quotes / on_fill / on_*).
template <typename LoggerT>
void run_zero_alloc_loop(LoggerT& logger) {
    SimulationConfig cfg;
    cfg.seed = 1;
    cfg.iterations = 0;
    cfg.quiet = true;
    // Hot-path alloc guard measures MM's tick_once steady-state allocs
    // against pre-built MD events; sim is used only as a host for the
    // matching engine that MM submits orders to. Pin to Legacy so we
    // don't seed synthetic orders into the pool during sim construction —
    // those allocations are before the guard window (and so wouldn't
    // count) but pinning keeps the test's regime byte-equal to M5/M6/M7.
    cfg.lob_model = LobModel::Legacy;
    MarketSimulator sim(cfg);
    Instrument ins = sim.instrument_meta();
    Ticks bp = ins.to_ticks(99.95);
    Ticks ap = ins.to_ticks(100.05);

    FixedQuoteStrategy strat(bp, ap, 5, 5);
    mme::MarketMakerT<FixedQuoteStrategy, LoggerT> mm(
        ins, RiskConfig{}, &strat, &logger);

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

TEST(HotPathAllocations, steady_state_emits_zero_allocations) {
    NullLogger logger;
    run_zero_alloc_loop(logger);
}

// C3-proper invariant: routing the same hot path through the SPSC binary
// logger must not allocate either. Construction (ring buffer, drain
// thread, file open + header write) happens outside the guard. Producer-
// side encoding uses a stack buffer + try_push only.
TEST(HotPathAllocations, spsc_logger_steady_state_zero_allocations) {
    const std::string path = (std::filesystem::temp_directory_path()
                              / "spsc_logger_zero_alloc.bin").string();
    std::remove(path.c_str());
    SpscLogger logger(path);
    run_zero_alloc_loop(logger);
    std::remove(path.c_str());
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
