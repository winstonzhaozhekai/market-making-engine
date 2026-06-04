#include "MarketSimulator.h"
#include "include/ItchParser.h"
#include "include/MdLogReader.h"
#include "include/SpscMdLogger.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
constexpr int64_t kBaseTimestampMs = 1700000000000LL;

constexpr uint64_t kSimOrderTag  = 2ULL << 48;
constexpr uint64_t kTradeIdTag   = 3ULL << 48;
constexpr uint64_t kItchOrderTag = 4ULL << 48;

std::chrono::system_clock::time_point from_millis(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}
} // namespace

MarketSimulator::MarketSimulator(const SimulationConfig& cfg)
    : config(cfg),
      instrument_(cfg.tick_size),
      instrument(cfg.instrument),
      mid_price_dollars(cfg.initial_price),
      spread_dollars(cfg.spread),
      volatility(cfg.volatility),
      rng(cfg.seed),
      sequence_number(0),
      simulation_clock(from_millis(kBaseTimestampMs + static_cast<int64_t>(cfg.seed) * 1000)),
      replay_index(0) {

    trades_buf_.reserve(4);
    mm_fills_buf_.reserve(8);

    if (config.mode == SimulationMode::Replay) {
        if (config.replay_log_path.empty()) {
            throw std::runtime_error("Replay mode requires a replay log path");
        }
        load_binary_event_log(config.replay_log_path);
        if (replay_events.empty()) {
            throw std::runtime_error("Replay log is empty: " + config.replay_log_path);
        }
        return;
    }

    if (config.mode == SimulationMode::ItchReplay) {
        if (!config.all_latencies_zero()) {
            throw std::runtime_error(
                "ItchReplay mode requires all-zero latencies in v1");
        }
        if (config.itch_log_path.empty()) {
            throw std::runtime_error("ItchReplay requires an itch_log_path");
        }
        if (config.itch_symbol.empty()) {
            throw std::runtime_error("ItchReplay requires an itch_symbol");
        }
        init_itch_replay();
        return;
    }

    if (!config.event_log_path.empty()) {
        event_logger_ = std::make_unique<SpscMdLogger>(config.event_log_path);
    }

    // M8: engage the discrete-event scheduler only when at least one stage
    // has non-zero latency. The all-zero case stays on the M6/M7 fast path
    // so the binary-log byte-equality + deterministic-replay invariants
    // hold without the scheduler being able to perturb them.
    if (!config.all_latencies_zero()) {
        latency_.emplace(LatencyState{
            mme::StageSampler(config.feed_latency),
            mme::StageSampler(config.ack_latency),
            mme::StageSampler(config.matching_latency),
            std::mt19937_64(config.latency_seed),
            0,
            {},
            {},
            {},
        });
    }

    if (config.lob_model == LobModel::QueueReactive) {
        init_queue_reactive();
    } else {
        initialize_order_book();
    }
}

MarketSimulator::~MarketSimulator() = default;

void MarketSimulator::initialize_order_book() {
    std::uniform_int_distribution<int> size_dist(1, 10);
    bid_levels_.reserve(5);
    ask_levels_.reserve(5);
    for (int i = 1; i <= 5; ++i) {
        double price_offset = i * spread_dollars / 2.0;
        Ticks bid_t = instrument_.to_ticks(mid_price_dollars - price_offset);
        Ticks ask_t = instrument_.to_ticks(mid_price_dollars + price_offset);
        bid_levels_.emplace_back(bid_t, size_dist(rng), generate_order_id(), current_time());
        ask_levels_.emplace_back(ask_t, size_dist(rng), generate_order_id(), current_time());
    }
}

MarketDataEvent MarketSimulator::generate_event() {
    if (!replay_events.empty()) {
        if (replay_index >= replay_events.size()) {
            throw std::out_of_range("Replay log exhausted");
        }
        return replay_events[replay_index++];
    }

    if (!latency_) {
        return produce_raw_md_event();
    }
    return generate_event_with_latency();
}

MarketDataEvent MarketSimulator::produce_raw_md_event() {
    trades_buf_.clear();
    mm_fills_buf_.clear();

    if (itch_) {
        // M10 ITCH replay: drive the matching engine off the next book-
        // affecting message on the tape. Resting flow is ITCH; MM-resting
        // flow (kMmOrderTag) intermingles in the same engine FIFO and
        // gets filled when ITCH `E` (Execute) injects IOC aggressors.
        step_itch_replay(trades_buf_, mm_fills_buf_);
    } else if (qr_) {
        // M9 QueueReactive path: brownian mid is skipped (price discovery
        // is implicit in HLR market-order-driven inside depletion +
        // refill). All synthetic orders rest in the matching engine; the
        // 20%-IOC stopgap is gone. MD level arrays are derived from
        // engine depth below (M9/3).
        step_queue_reactive(trades_buf_, mm_fills_buf_);
    } else {
        std::normal_distribution<> noise(0, volatility);
        mid_price_dollars += noise(rng);
        mid_price_dollars = std::max(mid_price_dollars, 0.01);

        update_order_book();
        simulate_trade_activity(trades_buf_, mm_fills_buf_);
    }

    auto event_creation_time = current_time();

    if (qr_ || itch_) {
        // Derive MD level arrays from the matching engine's authoritative
        // book state. Depth 0 is the actual top-of-book, so MM resting
        // inside the {HLR, ITCH} levels surfaces naturally; per-level
        // `size` is the aggregate qty across all orders at that price
        // (synthetic / ITCH makers + MM if it happens to sit at the
        // level); `order_id` reports the time-priority-front maker so
        // consumers that want a representative id can pick it up. We cap
        // depth at the configured HLR level count under QueueReactive to
        // keep MD shape close to the pre-M9 5-level convention; under
        // ItchReplay we use the same cap so strategy contracts on
        // num_levels stay stable across modes.
        const std::size_t cap = static_cast<std::size_t>(config.hlr.num_levels);
        bid_levels_.clear();
        ask_levels_.clear();
        const std::size_t n_bid =
            std::min<std::size_t>(matching_engine.num_levels(Side::BUY),  cap);
        const std::size_t n_ask =
            std::min<std::size_t>(matching_engine.num_levels(Side::SELL), cap);
        bid_levels_.reserve(n_bid);
        ask_levels_.reserve(n_ask);
        for (std::size_t d = 0; d < n_bid; ++d) {
            bid_levels_.emplace_back(
                matching_engine.price_at_depth(Side::BUY, d),
                matching_engine.qty_at_depth(Side::BUY, d),
                matching_engine.order_id_at_queue_pos(Side::BUY, d, 0),
                event_creation_time);
        }
        for (std::size_t d = 0; d < n_ask; ++d) {
            ask_levels_.emplace_back(
                matching_engine.price_at_depth(Side::SELL, d),
                matching_engine.qty_at_depth(Side::SELL, d),
                matching_engine.order_id_at_queue_pos(Side::SELL, d, 0),
                event_creation_time);
        }
    }

    std::vector<PartialFillEvent> partial_fills;
    for (const auto& fill : mm_fills_buf_) {
        if (fill.leaves_qty > 0) {
            partial_fills.push_back(PartialFillEvent{
                fill.order_id,
                fill.price,
                fill.fill_qty,
                fill.leaves_qty,
                fill.timestamp
            });
        }
    }

    // MD header: bid_levels_.front() is authoritative for both paths.
    // Legacy populates bid_levels_/ask_levels_ via update_order_book();
    // QueueReactive populates them above from engine depth at depth 0.
    MarketDataEvent event{
        instrument,
        bid_levels_.empty() ? Ticks{0} : bid_levels_.front().price,
        ask_levels_.empty() ? Ticks{0} : ask_levels_.front().price,
        bid_levels_.empty() ? 0 : bid_levels_.front().size,
        ask_levels_.empty() ? 0 : ask_levels_.front().size,
        bid_levels_,
        ask_levels_,
        std::move(trades_buf_),
        std::move(partial_fills),
        std::move(mm_fills_buf_),
        event_creation_time,
        ++sequence_number
    };

    if (event_logger_) {
        event_logger_->log_event(event);
    }

    trades_buf_.reserve(4);
    mm_fills_buf_.reserve(8);

    return event;
}

// Discrete-event production loop driving the three per-stage delays.
//
// Each pass of the inner loop advances sim_clock_ns by one production step
// (1 µs), lands any pending orders whose ack window has matured, produces a
// fresh raw MD event (which may already have generated fills against now-
// resting MM orders via simulate_trade_activity), then strips and defers
// those fills into match_queue. The raw MD is parked in feed_queue with a
// per-event feed-latency sample as its deliver_at. The function returns the
// head of feed_queue as soon as its deliver_at has matured, with any
// matched-and-visible fills attached. MM therefore observes:
//   - book state stale by ~feed_latency relative to current sim_clock
//   - orders posted ack_latency-deferred before they can be hit
//   - fills reported feed_latency + matching_latency after match time
MarketDataEvent MarketSimulator::generate_event_with_latency() {
    constexpr std::int64_t kProductionStepNs = 1000;  // 1 µs / event
    auto& L = *latency_;

    while (true) {
        L.sim_clock_ns += kProductionStepNs;

        drain_ack_queue();

        MarketDataEvent raw = produce_raw_md_event();

        // Defer mm_fills via matching latency. Strip both mm_fills and the
        // derived partial_fills from the raw event so MM cannot see them
        // until they mature.
        if (!raw.mm_fills.empty()) {
            for (auto& f : raw.mm_fills) {
                std::int64_t visible_at = L.sim_clock_ns + L.matching.sample(L.rng);
                L.match_queue.push_back(PendingFill{std::move(f), visible_at});
            }
            raw.mm_fills.clear();
            raw.partial_fills.clear();
        }

        // Schedule the raw event for feed delivery.
        std::int64_t deliver_at = L.sim_clock_ns + L.feed.sample(L.rng);
        L.feed_queue.push_back(PendingFeedEvent{std::move(raw), deliver_at});

        // Deliver the head of feed_queue if it has matured. Attach any
        // matched fills whose visibility window has also matured.
        if (L.feed_queue.front().deliver_time_ns <= L.sim_clock_ns) {
            auto pending = std::move(L.feed_queue.front());
            L.feed_queue.pop_front();

            while (!L.match_queue.empty() &&
                   L.match_queue.front().visible_time_ns <= L.sim_clock_ns) {
                auto pf = std::move(L.match_queue.front());
                L.match_queue.pop_front();
                if (pf.fill.leaves_qty > 0) {
                    pending.md.partial_fills.push_back(PartialFillEvent{
                        pf.fill.order_id,
                        pf.fill.price,
                        pf.fill.fill_qty,
                        pf.fill.leaves_qty,
                        pf.fill.timestamp,
                    });
                }
                pending.md.mm_fills.push_back(std::move(pf.fill));
            }

            return std::move(pending.md);
        }
    }
}

void MarketSimulator::drain_ack_queue() {
    auto& L = *latency_;
    while (!L.ack_queue.empty() &&
           L.ack_queue.front().land_time_ns <= L.sim_clock_ns) {
        auto po = std::move(L.ack_queue.front());
        L.ack_queue.pop_front();
        matching_engine.add_order(std::move(po.order), po.type);
    }
}

void MarketSimulator::simulate_trade_activity(std::vector<Trade>& trades, std::vector<FillEvent>& mm_fills) {
    std::uniform_real_distribution<> prob_dist(0.0, 1.0);
    std::uniform_int_distribution<> size_dist(1, 20);

    if (prob_dist(rng) < 0.2) {
        bool is_buy = prob_dist(rng) < 0.5;
        Side aggressor_side = is_buy ? Side::BUY : Side::SELL;
        auto& levels = is_buy ? ask_levels_ : bid_levels_;

        if (!levels.empty()) {
            int trade_size = size_dist(rng);
            Ticks sim_top_price = levels[0].price;
            Ticks trade_price = sim_top_price;
            bool  route_to_engine = true;

            // M8 queue-position model (Legacy-only path; QueueReactive
            // never reaches this function — see produce_raw_md_event).
            // When latency is engaged, treat the synthetic LOB at
            // sim_top_price as having unlimited queued size with full
            // time priority over MM at that price. MM is only hit when
            // MM's resting quote is strictly better than the synthetic
            // top. Without this the IOC pass-through sweeps MM at MM's
            // stale price even when MM is behind the displayed market —
            // produces an adverse-selection bump (more fills, worse
            // prices) at small latency that masks the monotone
            // fill-rate-vs-feed-latency signal. M9's queue-reactive LOB
            // subsumes this with explicit queue modeling, which is why
            // the gate is unreachable under QueueReactive.
            //
            // Zero-latency Legacy keeps M6/M7 semantics verbatim so the
            // binary-log byte-equality + deterministic-replay invariants
            // survive — the rule applies only when `latency_` is engaged.
            if (latency_) {
                Side mm_side = is_buy ? Side::SELL : Side::BUY;
                if (matching_engine.empty(mm_side)) {
                    route_to_engine = false;
                } else {
                    Ticks mm_best = matching_engine.best_price(mm_side);
                    bool mm_at_inside = is_buy
                        ? (mm_best < sim_top_price)
                        : (mm_best > sim_top_price);
                    if (mm_at_inside) {
                        trade_price = mm_best;
                    } else {
                        route_to_engine = false;
                    }
                }
            }

            uint64_t trade_id = kTradeIdTag | static_cast<uint64_t>(sequence_number + 1);
            auto ts = current_time();

            trades.emplace_back(Trade{
                aggressor_side,
                trade_price,
                trade_size,
                trade_id,
                ts
            });

            if (route_to_engine) {
                // Aggressor flows through the same add_order entry-point
                // as any other order — with IOC semantics, residual is
                // discarded rather than rested (matches the "random
                // market sweep" intent and unifies the engine API around
                // one path).
                Order aggressor(kTradeIdTag | trade_id, aggressor_side,
                                trade_price, trade_size, ts);
                auto fills = matching_engine.add_order(
                    std::move(aggressor), OrderType::IOC, trade_id).fills;
                mm_fills.insert(mm_fills.end(), fills.begin(), fills.end());
            }
        }
    }
}

OrderStatus MarketSimulator::submit_order(const Order& order, OrderType type) {
    if (!latency_) {
        return matching_engine.add_order(order, type).status;
    }
    // Gateway-ack model: tell MM the order was acknowledged immediately,
    // but defer landing in the matching engine by an ack-latency sample.
    // Counterparty trades arriving in (now, now + ack_sample) cannot hit
    // this order — that's the entire point of the stage.
    auto& L = *latency_;
    std::int64_t land_at = L.sim_clock_ns + L.ack.sample(L.rng);
    L.ack_queue.push_back(PendingOrder{order, type, land_at});
    return OrderStatus::ACKNOWLEDGED;
}

bool MarketSimulator::cancel_order(uint64_t order_id) {
    if (latency_) {
        // Pending (not-yet-landed) orders are cancellable at the gateway,
        // matching real-exchange behavior: a not-yet-acked order can be
        // pulled before it lands.
        auto& q = latency_->ack_queue;
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->order.order_id == order_id) {
                q.erase(it);
                return true;
            }
        }
    }
    return matching_engine.cancel_order(order_id);
}

bool MarketSimulator::amend_order(uint64_t order_id, Ticks new_price,
                                  int new_qty,
                                  std::chrono::system_clock::time_point ts) {
    if (latency_) {
        // Amend the pending order in-place if it hasn't landed yet. We
        // resample ack latency for the amended version so the stage
        // applies symmetrically to amends.
        auto& q = latency_->ack_queue;
        for (auto& po : q) {
            if (po.order.order_id == order_id) {
                po.order.price        = new_price;
                po.order.original_qty = new_qty;
                po.order.leaves_qty   = new_qty;
                po.order.updated_at   = ts;
                po.land_time_ns = latency_->sim_clock_ns
                                + latency_->ack.sample(latency_->rng);
                return true;
            }
        }
    }
    return matching_engine.amend_order(order_id, new_price, new_qty, ts);
}

void MarketSimulator::update_order_book() {
    std::uniform_real_distribution<> noise_dist(-0.001, 0.001);
    std::uniform_int_distribution<> size_change_dist(-2, 2);

    // Re-anchor each level around the (sub-tick) mid in dollars, then snap.
    // Without this, bid/ask levels drift far from mid_price, giving the
    // strategy stale market data and a permanently zero sigma estimate.
    for (std::size_t i = 0; i < bid_levels_.size(); ++i) {
        double base_offset = static_cast<double>(i + 1) * spread_dollars / 2.0;
        bid_levels_[i].price = instrument_.to_ticks(
            mid_price_dollars - base_offset + noise_dist(rng));
        bid_levels_[i].size = std::max(1, bid_levels_[i].size + size_change_dist(rng));
    }
    std::sort(bid_levels_.begin(), bid_levels_.end(),
              [](const OrderLevel& a, const OrderLevel& b) { return a.price > b.price; });

    for (std::size_t i = 0; i < ask_levels_.size(); ++i) {
        double base_offset = static_cast<double>(i + 1) * spread_dollars / 2.0;
        ask_levels_[i].price = instrument_.to_ticks(
            mid_price_dollars + base_offset + noise_dist(rng));
        ask_levels_[i].size = std::max(1, ask_levels_[i].size + size_change_dist(rng));
    }
    std::sort(ask_levels_.begin(), ask_levels_.end(),
              [](const OrderLevel& a, const OrderLevel& b) { return a.price < b.price; });
}

uint64_t MarketSimulator::generate_order_id() {
    return kSimOrderTag | ++sim_order_counter_;
}

// `simulation_clock` is a deterministic, monotonic synthetic clock — NOT real
// wall-clock time. It is initialized from `cfg.seed` and advances by exactly
// 1 ms on every call. Each `generate_event()` calls this multiple times (once
// per built timestamp), so a single market-data event spans several "ms" of
// simulator time. Implications:
//   - Same-seed runs produce byte-identical timestamps (replay determinism).
//   - Different seeds produce different absolute timestamps; absolute values
//     are not comparable across seeds.
//   - Risk rate-windows and stale-data checks are evaluated against this
//     synthetic clock, so their semantics are tied to *call frequency*, not
//     real elapsed time.
//   - This type uses `system_clock::time_point` only as a typed monotonic
//     counter. NTP slewing cannot affect it because `system_clock::now()` is
//     never called for these values. The type will be migrated when per-stage
//     latency lands (M8).
std::chrono::system_clock::time_point MarketSimulator::current_time() {
    simulation_clock += std::chrono::milliseconds(1);
    return simulation_clock;
}

// ---- M9 queue-reactive LOB wiring ---------------------------------------
//
// Synthetic-order bookkeeping invariant:
//   qr_->bid_side.queue_at(i)  ==  sum of leaves_qty of resting synthetic
//                                  orders at bid_level_ticks(i) in the
//                                  matching engine.
// Every synthetic order rests at exactly cfg.hlr.mean_limit_size units.
// LimitAdd push_back to fifo[level]; Cancel pop_back; MarketOrder consumes
// inside qty via an IOC and we walk SubmitResult.fills to update FIFOs +
// HLR.q. MM-side fills (kSimOrderTag bit clear) propagate to mm_fills.

Ticks MarketSimulator::bid_level_ticks(int level) const {
    return qr_->ref_ticks - Ticks{static_cast<Ticks>(level + 1)};
}
Ticks MarketSimulator::ask_level_ticks(int level) const {
    return qr_->ref_ticks + Ticks{static_cast<Ticks>(level + 1)};
}
int MarketSimulator::bid_level_of_price(Ticks px) const {
    const Ticks off = qr_->ref_ticks - px;
    const int   lvl = static_cast<int>(off) - 1;
    if (lvl < 0 || lvl >= config.hlr.num_levels) return -1;
    return lvl;
}
int MarketSimulator::ask_level_of_price(Ticks px) const {
    const Ticks off = px - qr_->ref_ticks;
    const int   lvl = static_cast<int>(off) - 1;
    if (lvl < 0 || lvl >= config.hlr.num_levels) return -1;
    return lvl;
}

void MarketSimulator::init_queue_reactive() {
    // Floor-round initial_queue to whole orders of mean_limit_size so the
    // HLR.q ≡ engine-depth invariant holds exactly after seeding. The
    // primitive's `step()` keeps the invariant from drifting because
    // adds/cancels move in unit_size increments and MarketOrder
    // consumption is reconciled in step_queue_reactive() below.
    mme::HLRConfig hlr_cfg = config.hlr;
    const int unit_size = hlr_cfg.mean_limit_size;
    for (int i = 0; i < hlr_cfg.num_levels; ++i) {
        hlr_cfg.initial_queue[i] =
            (hlr_cfg.initial_queue[i] / unit_size) * unit_size;
    }

    qr_.emplace(QueueReactiveState{
        mme::HLRSide(Side::BUY,  hlr_cfg),
        mme::HLRSide(Side::SELL, hlr_cfg),
        std::mt19937_64(config.lob_seed),
        {},
        {},
        {},
        instrument_.to_ticks(mid_price_dollars),
    });

    auto& qr = *qr_;
    qr.event_buf.reserve(16);
    for (int i = 0; i < hlr_cfg.num_levels; ++i) {
        qr.bid_orders[i].clear();
        qr.ask_orders[i].clear();
    }

    for (int level = 0; level < hlr_cfg.num_levels; ++level) {
        const int n_orders = hlr_cfg.initial_queue[level] / unit_size;
        for (int k = 0; k < n_orders; ++k) {
            const auto ts = current_time();
            {
                const uint64_t id = generate_order_id();
                Order o(id, Side::BUY, bid_level_ticks(level), unit_size, ts);
                matching_engine.add_order(std::move(o), OrderType::POST_ONLY);
                qr.bid_orders[level].push_back(id);
            }
            {
                const uint64_t id = generate_order_id();
                Order o(id, Side::SELL, ask_level_ticks(level), unit_size, ts);
                matching_engine.add_order(std::move(o), OrderType::POST_ONLY);
                qr.ask_orders[level].push_back(id);
            }
        }
    }
}

void MarketSimulator::step_queue_reactive(std::vector<Trade>&     trades,
                                          std::vector<FillEvent>& mm_fills) {
    constexpr std::int64_t kStepNs = 1000;  // 1 µs, matches M8 production step
    auto& qr = *qr_;

    qr.event_buf.clear();
    qr.bid_side.step(kStepNs, qr.rng, qr.event_buf);
    qr.ask_side.step(kStepNs, qr.rng, qr.event_buf);

    for (const auto& ev : qr.event_buf) {
        switch (ev.kind) {
        case mme::HLREventKind::LimitAdd: {
            const Ticks px = (ev.side == Side::BUY)
                ? bid_level_ticks(ev.level)
                : ask_level_ticks(ev.level);
            const uint64_t id = generate_order_id();
            Order o(id, ev.side, px, ev.qty, current_time());
            matching_engine.add_order(std::move(o), OrderType::POST_ONLY);
            auto& fifo = (ev.side == Side::BUY)
                ? qr.bid_orders[ev.level]
                : qr.ask_orders[ev.level];
            fifo.push_back(id);
            break;
        }
        case mme::HLREventKind::Cancel: {
            auto& fifo = (ev.side == Side::BUY)
                ? qr.bid_orders[ev.level]
                : qr.ask_orders[ev.level];
            if (!fifo.empty()) {
                const uint64_t id = fifo.back();
                fifo.pop_back();
                matching_engine.cancel_order(id);
            }
            // If fifo empty here, the resting order was already consumed
            // by a same-step MarketOrder above (rare with Bernoulli draws
            // at 1µs step rates). HLR.q was decremented at sample time so
            // a stale Cancel just drops without side-effect.
            break;
        }
        case mme::HLREventKind::MarketOrder: {
            // Aggressor is the OPPOSITE side of `ev.side` (a bid-side
            // MarketOrder event depletes resting BUY queue → aggressor
            // SELLs). Submit a marketable IOC; the engine matches by
            // price-time priority and routes any MM-resting fills to
            // `mm_fills` while we settle synthetic-resting fills against
            // the per-level FIFO.
            const Side aggressor = (ev.side == Side::BUY) ? Side::SELL : Side::BUY;
            const Ticks ioc_px = (aggressor == Side::BUY)
                ? ask_level_ticks(config.hlr.num_levels - 1)  // sweep up to deepest ask
                : bid_level_ticks(config.hlr.num_levels - 1); // sweep down to deepest bid

            const uint64_t trade_id =
                kTradeIdTag | static_cast<uint64_t>(sequence_number + 1);
            const auto     ts       = current_time();
            Order ioc(kTradeIdTag | trade_id, aggressor, ioc_px, ev.qty, ts);

            auto result = matching_engine.add_order(
                std::move(ioc), OrderType::IOC, trade_id);

            // The trade event emits at the inside price the IOC hit first.
            // We summarize aggressor-side activity as a single Trade with
            // the cumulative filled qty; the per-maker fills below carry
            // per-leg pricing.
            int total_filled = 0;
            Ticks trade_px = ioc_px;
            bool  first    = true;
            for (const auto& fill : result.fills) {
                total_filled += fill.fill_qty;
                if (first) { trade_px = fill.price; first = false; }
            }
            if (total_filled > 0) {
                trades.emplace_back(Trade{
                    aggressor, trade_px, total_filled, trade_id, ts
                });
            }

            // Settle each fill against either the synthetic FIFO or MM.
            constexpr uint64_t kSimMask = kSimOrderTag;
            int synthetic_consumed_units = 0;
            for (const auto& fill : result.fills) {
                const bool is_synthetic = (fill.order_id & kSimMask) != 0;
                if (is_synthetic) {
                    synthetic_consumed_units += fill.fill_qty;
                    // Find which HLR level this synthetic order was at,
                    // and remove from FIFO if fully filled.
                    const int level = (fill.side == Side::BUY)
                        ? bid_level_of_price(fill.price)
                        : ask_level_of_price(fill.price);
                    if (level >= 0 && fill.leaves_qty == 0) {
                        auto& fifo = (fill.side == Side::BUY)
                            ? qr.bid_orders[level]
                            : qr.ask_orders[level];
                        for (auto it = fifo.begin(); it != fifo.end(); ++it) {
                            if (*it == fill.order_id) {
                                fifo.erase(it);
                                break;
                            }
                        }
                    }
                } else {
                    mm_fills.push_back(fill);
                }
            }

            // Reconcile HLR.q with what actually happened. The primitive
            // pre-decremented q[0] by ev.qty assuming all consumption
            // came from synthetic at level 0. If MM was at the inside,
            // some of that ev.qty was absorbed by MM and synthetic at
            // level 0 should not have moved. Refund the MM portion to
            // preserve the invariant
            //   HLR.q[level] == resting synthetic units at that level.
            const int mm_absorbed = ev.qty - synthetic_consumed_units;
            if (mm_absorbed > 0) {
                auto& hlr_side = (ev.side == Side::BUY)
                    ? qr.bid_side : qr.ask_side;
                hlr_side.on_external_increment(0, mm_absorbed);
            }
            break;
        }
        }
    }
}

void MarketSimulator::load_binary_event_log(const std::string& path) {
    MdLogReader reader(path);
    while (auto ev = reader.next()) {
        replay_events.push_back(std::move(*ev));
    }
    if (!replay_events.empty()) {
        sequence_number = replay_events.back().sequence_number;
        simulation_clock = replay_events.back().timestamp;
    }
}

// ---- M10 ITCH replay wiring ---------------------------------------------
//
// Engine-id discriminator:
//   kMmOrderTag   (1 << 48)  — MM orders submitted via submit_order
//   kSimOrderTag  (2 << 48)  — Legacy / M9 QueueReactive synthetic orders
//   kItchOrderTag (4 << 48)  — ITCH tape replay orders (this milestone)
//
// MM queue position vs ITCH-resting orders falls out of the matching
// engine's FIFO insertion order automatically: whoever called
// add_order() first for a given price level sits earlier in the queue.
//
// For ITCH `E` (Execute) messages, we inject a same-price IOC aggressor
// from the opposite side for the executed_shares amount. The engine
// matches by price-time priority; if MM is at the same price with
// earlier queue position than the named ITCH ref, MM gets filled first.
// We tag-split SubmitResult.fills:
//   - MM fills (kMmOrderTag bit set)   → mm_fills (visible to MM/PnL)
//   - ITCH fills (kItchOrderTag bit set) → consume from the named ref;
//     if the engine consumed fewer ITCH-side shares than ITCH expected
//     (because MM absorbed some), we amend the named ref down by the
//     residual to keep our mirror in lockstep with the exchange's view.
//
// `C` (Execute With Price), `X` (Cancel partial), `D` (Delete), `U`
// (Replace) follow direct engine ops (amend_order / cancel_order). `P`
// (Trade Non-Cross) is a hidden-liquidity print that does not touch the
// book; we emit a Trade event for the strategy's tape-aware logic.

Ticks MarketSimulator::itch_price_to_ticks(std::uint32_t price_x10000) const {
    // ITCH price encoding is integer × 1e-4 dollars. We round to the
    // nearest engine tick — this lossy-rounds sub-tick prices (which
    // appear on `C` (Execute With Price) and on penny-pilot symbols
    // with sub-penny ticks), but for the default tick_size = 0.01
    // regime every standard ITCH price hits an exact tick.
    const double dollars = static_cast<double>(price_x10000) * 1e-4;
    return instrument_.to_ticks(dollars);
}

void MarketSimulator::init_itch_replay() {
    // Load the whole tape into memory. Test tapes are tiny; a full Nasdaq
    // trading day is multi-GB and warrants an mmap swap in a follow-on
    // (perf-only) commit. The walker is content-addressed by `cursor` so
    // swapping the backing store is opaque to step_itch_replay.
    std::ifstream in(config.itch_log_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(
            "ItchReplay: failed to open " + config.itch_log_path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);

    ItchReplayState st{};
    st.tape.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(st.tape.data()), size);
        if (!in) {
            throw std::runtime_error(
                "ItchReplay: short read on " + config.itch_log_path);
        }
    }

    // Symbol field on tape is 8-byte ASCII, space-padded.
    {
        const std::string& s = config.itch_symbol;
        if (s.size() > 8) {
            throw std::runtime_error("ItchReplay: symbol >8 chars");
        }
        st.symbol.fill(' ');
        std::memcpy(st.symbol.data(), s.data(), s.size());
    }

    // Pre-scan: walk R (Stock Directory) messages to resolve stock_locate.
    // We do NOT advance the playback cursor — step_itch_replay re-walks
    // R/S/H during normal playback and ignores them.
    std::size_t scan = 0;
    while (scan < st.tape.size() && !st.filter_resolved) {
        mme::itch::TapeFrame frame{};
        std::size_t consumed = 0;
        if (!mme::itch::next_frame(st.tape.data() + scan,
                                   st.tape.size() - scan,
                                   &frame, &consumed)) {
            break;
        }
        if (frame.type == 'R') {
            const auto m = mme::itch::decode_stock_directory(frame.payload);
            if (std::memcmp(m.stock, st.symbol.data(), 8) == 0) {
                st.stock_locate_filter = m.hdr.stock_locate;
                st.filter_resolved     = true;
            }
        }
        scan += consumed;
    }
    if (!st.filter_resolved) {
        throw std::runtime_error(
            "ItchReplay: symbol not found on tape: " + config.itch_symbol);
    }

    itch_.emplace(std::move(st));
}

namespace {

// Helper: pop the engine-id matching `engine_id` from FIFO `fifo`.
// Returns true if removed. Used for MM-absorb residual reconciliation.
template <typename Fifo>
bool fifo_erase(Fifo& fifo, std::uint64_t engine_id) {
    for (auto it = fifo.begin(); it != fifo.end(); ++it) {
        if (*it == engine_id) {
            fifo.erase(it);
            return true;
        }
    }
    return false;
}

}  // namespace

void MarketSimulator::step_itch_replay(std::vector<Trade>&     trades,
                                       std::vector<FillEvent>& mm_fills) {
    auto& st = *itch_;

    // Walk forward consuming non-matching / metadata messages until we
    // produce one MD-event-worth of state change for the strategy.
    while (true) {
        if (st.cursor >= st.tape.size()) {
            throw std::out_of_range("ItchReplay: tape exhausted");
        }

        mme::itch::TapeFrame frame{};
        std::size_t consumed = 0;
        if (!mme::itch::next_frame(st.tape.data() + st.cursor,
                                   st.tape.size() - st.cursor,
                                   &frame, &consumed)) {
            throw std::out_of_range("ItchReplay: malformed frame at cursor");
        }
        st.cursor += consumed;

        const char type = frame.type;

        // Header is at a stable offset for every typed message. Cheap
        // pre-check so we can filter on stock_locate before doing the
        // (still-cheap) full decode.
        const auto hdr = mme::itch::decode_header(frame.payload);
        st.last_timestamp_ns = static_cast<std::int64_t>(hdr.timestamp_ns);

        // S/H/R don't affect the book; consume and continue silently.
        // (R was processed during init_itch_replay's pre-scan.)
        if (type == 'S' || type == 'H' || type == 'R') {
            continue;
        }

        // Filter: only book-affecting messages for our symbol.
        if (hdr.stock_locate != st.stock_locate_filter) {
            continue;
        }

        const auto ts = current_time();

        switch (type) {
        case 'A': {
            const auto m = mme::itch::decode_add_order(frame.payload);
            const Side  side = (m.side == 'B') ? Side::BUY : Side::SELL;
            const Ticks px   = itch_price_to_ticks(m.price_x10000);
            const std::uint64_t eng_id = kItchOrderTag | m.order_ref;
            Order o(eng_id, side, px, static_cast<int>(m.shares), ts);
            matching_engine.add_order(std::move(o), OrderType::POST_ONLY);
            st.ref_price[m.order_ref] = ItchRefInfo{side, px};
            return;
        }
        case 'F': {
            const auto m = mme::itch::decode_add_order_attributed(frame.payload);
            const Side  side = (m.side == 'B') ? Side::BUY : Side::SELL;
            const Ticks px   = itch_price_to_ticks(m.price_x10000);
            const std::uint64_t eng_id = kItchOrderTag | m.order_ref;
            Order o(eng_id, side, px, static_cast<int>(m.shares), ts);
            matching_engine.add_order(std::move(o), OrderType::POST_ONLY);
            st.ref_price[m.order_ref] = ItchRefInfo{side, px};
            return;
        }
        case 'E': {
            const auto m = mme::itch::decode_order_executed(frame.payload);
            auto it = st.ref_price.find(m.order_ref);
            if (it == st.ref_price.end()) {
                // Order we never saw an add for (replay started mid-day).
                // Silently skip the execution; emit no MD event for this
                // step and continue to the next message.
                continue;
            }
            const Side  resting_side = it->second.side;
            const Ticks resting_px   = it->second.price;
            const Side  aggressor    = (resting_side == Side::BUY)
                                           ? Side::SELL : Side::BUY;
            const std::uint64_t eng_id   = kItchOrderTag | m.order_ref;
            const std::uint64_t trade_id =
                kTradeIdTag | static_cast<std::uint64_t>(m.match_number);

            // Inject IOC aggressor at the resting price. Engine matches
            // by price-time; MM at earlier queue position absorbs first.
            Order ioc(trade_id, aggressor, resting_px,
                      static_cast<int>(m.executed_shares), ts);
            auto result = matching_engine.add_order(
                std::move(ioc), OrderType::IOC, trade_id);

            int itch_consumed_units = 0;
            int mm_absorbed_units   = 0;
            for (const auto& fill : result.fills) {
                if ((fill.order_id & kItchOrderTag) != 0) {
                    itch_consumed_units += fill.fill_qty;
                } else {
                    mm_fills.push_back(fill);
                    mm_absorbed_units += fill.fill_qty;
                }
            }
            // Emit a single Trade summarizing the aggressor side at the
            // resting price (matches the M9 convention).
            const int total_filled = itch_consumed_units + mm_absorbed_units;
            if (total_filled > 0) {
                trades.emplace_back(Trade{
                    aggressor, resting_px, total_filled, trade_id, ts
                });
            }

            // Reconcile mirror with ITCH's view: ITCH expects the named
            // ref to have lost `executed_shares`; the engine actually
            // removed `itch_consumed_units` from it (MM absorbed the
            // rest before the IOC reached it). Amend the named ref down
            // by `mm_absorbed_units` to close the gap. amend with
            // new_qty <= leaves_qty preserves queue position.
            if (mm_absorbed_units > 0 && matching_engine.contains(eng_id)) {
                const int leaves = matching_engine.leaves_qty_of(eng_id);
                const int target = std::max(0, leaves - mm_absorbed_units);
                if (target == 0) {
                    matching_engine.cancel_order(eng_id);
                } else {
                    matching_engine.amend_order(eng_id, resting_px, target, ts);
                }
            }
            // If the engine fully consumed the ITCH ref, drop the map
            // entry so a stale future message can't double-resolve it.
            if (!matching_engine.contains(eng_id)) {
                st.ref_price.erase(it);
            }
            return;
        }
        case 'C': {
            const auto m = mme::itch::decode_order_executed_with_price(frame.payload);
            auto it = st.ref_price.find(m.order_ref);
            if (it == st.ref_price.end()) {
                continue;
            }
            // Hidden-price execution: v1 amend-only path. Does not
            // inject visible-book IOC, so MM is not affected by `C`
            // messages. (See the M10 Deferred section in ROADMAP.)
            const Ticks resting_px = it->second.price;
            const Side  resting_side = it->second.side;
            const Side  aggressor    = (resting_side == Side::BUY)
                                           ? Side::SELL : Side::BUY;
            const std::uint64_t eng_id   = kItchOrderTag | m.order_ref;
            const std::uint64_t trade_id =
                kTradeIdTag | static_cast<std::uint64_t>(m.match_number);
            const int leaves = matching_engine.leaves_qty_of(eng_id);
            if (leaves > 0) {
                const int target =
                    std::max(0, leaves - static_cast<int>(m.executed_shares));
                if (target == 0) {
                    matching_engine.cancel_order(eng_id);
                    st.ref_price.erase(it);
                } else {
                    matching_engine.amend_order(eng_id, resting_px, target, ts);
                }
            }
            // Emit a Trade at the explicit execution_price (NOT the
            // resting price), since the print is what tape consumers
            // see. The aggressor side is the opposite of the resting
            // order.
            if (m.printable == 'Y' && m.executed_shares > 0) {
                trades.emplace_back(Trade{
                    aggressor,
                    itch_price_to_ticks(m.execution_price_x10000),
                    static_cast<int>(m.executed_shares),
                    trade_id, ts
                });
            }
            return;
        }
        case 'X': {
            const auto m = mme::itch::decode_order_cancel(frame.payload);
            auto it = st.ref_price.find(m.order_ref);
            if (it == st.ref_price.end()) continue;
            const std::uint64_t eng_id = kItchOrderTag | m.order_ref;
            const int leaves = matching_engine.leaves_qty_of(eng_id);
            if (leaves <= 0) continue;
            const int target =
                std::max(0, leaves - static_cast<int>(m.canceled_shares));
            if (target == 0) {
                matching_engine.cancel_order(eng_id);
                st.ref_price.erase(it);
            } else {
                matching_engine.amend_order(eng_id, it->second.price,
                                            target, ts);
            }
            return;
        }
        case 'D': {
            const auto m = mme::itch::decode_order_delete(frame.payload);
            const std::uint64_t eng_id = kItchOrderTag | m.order_ref;
            matching_engine.cancel_order(eng_id);
            st.ref_price.erase(m.order_ref);
            return;
        }
        case 'U': {
            const auto m = mme::itch::decode_order_replace(frame.payload);
            const std::uint64_t orig_eng_id = kItchOrderTag | m.orig_order_ref;
            auto it = st.ref_price.find(m.orig_order_ref);
            Side side = Side::BUY;
            if (it != st.ref_price.end()) {
                side = it->second.side;
                st.ref_price.erase(it);
            }
            matching_engine.cancel_order(orig_eng_id);

            const Ticks new_px = itch_price_to_ticks(m.new_price_x10000);
            const std::uint64_t new_eng_id = kItchOrderTag | m.new_order_ref;
            Order o(new_eng_id, side, new_px,
                    static_cast<int>(m.new_shares), ts);
            matching_engine.add_order(std::move(o), OrderType::POST_ONLY);
            st.ref_price[m.new_order_ref] = ItchRefInfo{side, new_px};
            return;
        }
        case 'P': {
            // Hidden trade: no book change. Emit a Trade event so PnL /
            // adverse-selection tracking has the print in the tape.
            const auto m = mme::itch::decode_trade_non_cross(frame.payload);
            const Side aggressor = (m.side == 'B') ? Side::BUY : Side::SELL;
            const std::uint64_t trade_id =
                kTradeIdTag | static_cast<std::uint64_t>(m.match_number);
            trades.emplace_back(Trade{
                aggressor, itch_price_to_ticks(m.price_x10000),
                static_cast<int>(m.shares), trade_id, ts
            });
            return;
        }
        default:
            // Unknown / fast-skipped types: keep scanning.
            continue;
        }
    }
}
