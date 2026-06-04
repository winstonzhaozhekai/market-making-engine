#ifndef MARKET_SIMULATOR_H
#define MARKET_SIMULATOR_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "MarketDataEvent.h"
#include "MatchingEngine.h"
#include "include/Instrument.h"
#include "include/LatencyScheduler.h"
#include "include/QueueReactiveLob.h"
#include "include/SimulationConfig.h"

class SpscMdLogger;

class MarketSimulator {
public:
    explicit MarketSimulator(const SimulationConfig& config);
    ~MarketSimulator();
    MarketDataEvent generate_event();

    // `type` is the realistic exchange order-type flag. MM-side callers
    // pass POST_ONLY; future LIMIT/IOC counterparty agents land in M9.
    OrderStatus submit_order(const Order& order, OrderType type);
    bool cancel_order(uint64_t order_id);
    bool amend_order(uint64_t order_id, Ticks new_price, int new_qty,
                     std::chrono::system_clock::time_point ts);
    const MatchingEngine& get_matching_engine() const { return matching_engine; }
    const Instrument& instrument_meta() const { return instrument_; }

private:
    SimulationConfig config;
    Instrument instrument_;
    std::string instrument;
    // Sub-tick random walk lives in dollars; emitted prices snap to ticks.
    double mid_price_dollars;
    double spread_dollars;
    double volatility;
    std::vector<OrderLevel> bid_levels_;
    std::vector<OrderLevel> ask_levels_;
    MatchingEngine matching_engine;
    std::mt19937 rng;
    int64_t sequence_number;
    uint64_t sim_order_counter_ = 0;
    std::chrono::system_clock::time_point simulation_clock;
    std::unique_ptr<SpscMdLogger> event_logger_;
    std::vector<MarketDataEvent> replay_events;
    std::size_t replay_index;

    std::vector<Trade> trades_buf_;
    std::vector<FillEvent> mm_fills_buf_;

    // ---- M8 per-stage latency state ------------------------------------
    // Engaged only when !config.all_latencies_zero(); nullopt otherwise so
    // the M6/M7 byte-equality fast path is preserved exactly.
    struct PendingOrder {
        Order        order;
        OrderType    type;
        std::int64_t land_time_ns;
    };
    struct PendingFeedEvent {
        MarketDataEvent md;
        std::int64_t    deliver_time_ns;
    };
    struct PendingFill {
        FillEvent    fill;
        std::int64_t visible_time_ns;
    };
    struct LatencyState {
        mme::StageSampler feed;
        mme::StageSampler ack;
        mme::StageSampler matching;
        std::mt19937_64   rng;
        std::int64_t      sim_clock_ns = 0;
        std::deque<PendingOrder>     ack_queue;
        std::deque<PendingFeedEvent> feed_queue;
        std::deque<PendingFill>      match_queue;
    };
    std::optional<LatencyState> latency_;

    // ---- M9 queue-reactive LOB state -----------------------------------
    // Engaged only when config.lob_model == LobModel::QueueReactive. The
    // HLR primitive (`include/QueueReactiveLob.h`) is the stochastic
    // generator; this struct owns the wiring state that maps emitted
    // HLREvents onto add_order / cancel_order / IOC calls against the
    // matching engine, plus the per-side per-level FIFO of synthetic
    // order ids that lets Cancel events pick a concrete order to pull.
    //
    // Invariant: hlr_side.queue_at(level) equals the total resting
    // synthetic qty at the corresponding price in the engine. Every
    // synthetic order rests at exactly `cfg.hlr.mean_limit_size` units;
    // LimitAdd push_back, Cancel pop_back, MarketOrder consumes the
    // head-of-queue via the engine's price-time-priority IOC match.
    struct QueueReactiveState {
        mme::HLRSide                bid_side;
        mme::HLRSide                ask_side;
        std::mt19937_64             rng;
        std::vector<mme::HLREvent>  event_buf;
        using LevelFifo = std::deque<std::uint64_t>;
        std::array<LevelFifo, mme::HLRConfig::kMaxLevels> bid_orders;
        std::array<LevelFifo, mme::HLRConfig::kMaxLevels> ask_orders;
        // Reference tick price. Bid level i sits at ref_ticks - (i+1);
        // ask level i sits at ref_ticks + (i+1). Fixed for v1 — mid drift
        // emerges from MarketOrder-driven inside depletion + refill.
        Ticks                        ref_ticks{0};
    };
    std::optional<QueueReactiveState> qr_;

    // ---- M10 ITCH replay state -----------------------------------------
    // Engaged only when `config.mode == SimulationMode::ItchReplay`. The
    // tape is loaded fully into memory on construction (small test tapes
    // are ~kB; a full Nasdaq trading day is multi-GB and would warrant an
    // mmap swap in a follow-on commit). `cursor` walks the buffer
    // length-prefix at a time via mme::itch::next_frame.
    //
    // `stock_locate_filter` is learned from the first R (Stock Directory)
    // message whose `stock` field matches `config.itch_symbol`. All
    // messages with a different stock_locate are fast-skipped.
    //
    // Engine ids for ITCH-resting orders use `kItchOrderTag | itch_ref`.
    // `ref_price` maps ITCH ref → (side, ticks) so an E (Execute) message
    // can submit a price-correct IOC injection at the resting price.
    struct ItchRefInfo {
        Side  side;
        Ticks price;
    };
    struct ItchReplayState {
        std::vector<std::uint8_t>                      tape;
        std::size_t                                    cursor = 0;
        std::uint16_t                                  stock_locate_filter = 0;
        bool                                           filter_resolved = false;
        std::array<char, 8>                            symbol{};
        std::int64_t                                   last_timestamp_ns = 0;
        std::unordered_map<std::uint64_t, ItchRefInfo> ref_price;
    };
    std::optional<ItchReplayState> itch_;

    // Produces a fresh raw MD event at current simulation time. Used by
    // both the zero-latency fast path (returned directly) and the M8 path
    // (enqueued in feed_queue; mm_fills stripped + deferred via match_queue).
    MarketDataEvent produce_raw_md_event();
    MarketDataEvent generate_event_with_latency();
    void drain_ack_queue();

    void initialize_order_book();
    void update_order_book();
    void simulate_trade_activity(std::vector<Trade>& trades, std::vector<FillEvent>& mm_fills);

    // ---- M9 queue-reactive LOB helpers ---------------------------------
    void init_queue_reactive();
    void step_queue_reactive(std::vector<Trade>&     trades,
                             std::vector<FillEvent>& mm_fills);

    // ---- M10 ITCH replay helpers ---------------------------------------
    void init_itch_replay();
    // Advances the cursor to the next matching-symbol book-affecting
    // message, applies it to the matching engine, and populates `trades`
    // / `mm_fills` with the consequences. Throws std::out_of_range when
    // the tape is exhausted (mirrors the SimulationMode::Replay
    // contract).
    void step_itch_replay(std::vector<Trade>&     trades,
                          std::vector<FillEvent>& mm_fills);
    Ticks itch_price_to_ticks(std::uint32_t price_x10000) const;
    Ticks bid_level_ticks(int level) const;
    Ticks ask_level_ticks(int level) const;
    int   bid_level_of_price(Ticks px) const;
    int   ask_level_of_price(Ticks px) const;

    uint64_t generate_order_id();
    std::chrono::system_clock::time_point current_time();
    void load_binary_event_log(const std::string& path);
};

#endif // MARKET_SIMULATOR_H
