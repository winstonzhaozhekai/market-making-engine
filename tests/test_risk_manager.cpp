#include <gtest/gtest.h>
#include <chrono>
#include "include/RiskManager.h"
#include "include/Instrument.h"

namespace {

constexpr double EPS = 1e-6;
const Instrument kIns{0.01};
Ticks T(double dollars) { return kIns.to_ticks(dollars); }

using time_point = std::chrono::system_clock::time_point;

time_point base_time() {
    return std::chrono::system_clock::from_time_t(1000000);
}

time_point offset_ms(int ms) {
    return base_time() + std::chrono::milliseconds(ms);
}

MarketDataEvent make_md(double bid, double ask, time_point ts, int64_t seq = 1) {
    MarketDataEvent md;
    md.instrument = "TEST";
    md.best_bid_price = T(bid);
    md.best_ask_price = T(ask);
    md.best_bid_size = 100;
    md.best_ask_size = 100;
    md.bid_levels.emplace_back(T(bid), 100, 1ULL, ts);
    md.ask_levels.emplace_back(T(ask), 100, 2ULL, ts);
    md.timestamp = ts;
    md.sequence_number = seq;
    return md;
}

// ============================================================
// Individual rule tests (11)
// ============================================================

TEST(RiskManager, test_max_net_position_normal) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Normal);
    EXPECT_TRUE(rm.is_quoting_allowed());
}

TEST(RiskManager, test_max_net_position_warning) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    for (int i = 0; i < 80; ++i)
        acct.on_fill(Side::BUY, T(100.0), 1, true);
    auto md = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Warning);
    EXPECT_TRUE(rm.is_quoting_allowed());
}

TEST(RiskManager, test_max_net_position_breached) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Breached);
    EXPECT_FALSE(rm.is_quoting_allowed());
}

TEST(RiskManager, test_max_notional_exposure_breached) {
    RiskConfig cfg;
    cfg.max_notional_exposure = 5000.0;
    cfg.max_net_position = 10000;
    RiskManager rm(cfg);
    Accounting acct(1000000.0);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Breached);
}

TEST(RiskManager, test_max_drawdown_breached) {
    RiskConfig cfg;
    cfg.max_drawdown = 100.0;
    cfg.max_net_position = 100000;
    cfg.max_notional_exposure = 1e9;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    acct.on_fill(Side::BUY, T(100.0), 10, true);
    acct.mark_to_market(89.0);
    auto md2 = make_md(88.95, 89.05, offset_ms(100), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 89.0), RiskState::Breached);
    EXPECT_GT(rm.current_drawdown(), 100.0);
}

TEST(RiskManager, test_max_drawdown_hwm_tracks) {
    RiskConfig cfg;
    cfg.max_drawdown = 10000.0;
    cfg.max_net_position = 100000;
    cfg.max_notional_exposure = 1e9;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    EXPECT_NEAR(rm.high_water_mark(), 0.0, EPS);
    acct.on_fill(Side::BUY, T(100.0), 10, true);
    acct.mark_to_market(110.0);
    auto md2 = make_md(109.95, 110.05, offset_ms(100), 2);
    rm.evaluate(acct, md2, 110.0);
    EXPECT_GT(rm.high_water_mark(), 0.0);
    double hwm = rm.high_water_mark();
    acct.mark_to_market(105.0);
    auto md3 = make_md(104.95, 105.05, offset_ms(200), 3);
    rm.evaluate(acct, md3, 105.0);
    EXPECT_EQ(rm.high_water_mark(), hwm);
}

TEST(RiskManager, test_max_quote_rate_breached) {
    RiskConfig cfg;
    cfg.max_quotes_per_second = 5.0;
    cfg.rate_window_seconds = 1.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto ts = base_time();
    for (int i = 0; i < 5; ++i)
        rm.record_quote(ts);
    auto md = make_md(100.0, 100.10, ts);
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Breached);
}

TEST(RiskManager, test_max_cancel_rate_breached) {
    RiskConfig cfg;
    cfg.max_cancels_per_second = 5.0;
    cfg.rate_window_seconds = 1.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto ts = base_time();
    for (int i = 0; i < 5; ++i)
        rm.record_cancel(ts);
    auto md = make_md(100.0, 100.10, ts);
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Breached);
}

TEST(RiskManager, test_stale_market_data_breached) {
    RiskConfig cfg;
    cfg.max_stale_data_ms = 1000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    auto md2 = make_md(100.0, 100.10, offset_ms(2000), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 100.05), RiskState::Breached);
}

TEST(RiskManager, test_stale_market_data_first_tick) {
    RiskConfig cfg;
    cfg.max_stale_data_ms = 100.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md, 100.05), RiskState::Normal);
}

TEST(RiskManager, test_max_quote_spread_breached) {
    RiskConfig cfg;
    cfg.max_quote_spread = 0.10;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md = make_md(100.0, 100.20, base_time());
    EXPECT_EQ(rm.evaluate(acct, md, 100.10), RiskState::Breached);
}

// ============================================================
// State machine tests (5)
// ============================================================

TEST(RiskManager, test_state_normal_to_warning) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md1 = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md1, 100.05), RiskState::Normal);
    acct.on_fill(Side::BUY, T(100.0), 85, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(100), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 100.05), RiskState::Warning);
}

TEST(RiskManager, test_state_normal_to_breached) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md1 = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md1, 100.05), RiskState::Normal);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(100), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 100.05), RiskState::Breached);
}

TEST(RiskManager, test_breached_requires_cooldown) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.cooldown_seconds = 5.0;
    cfg.max_stale_data_ms = 100000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md1 = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md1, 100.05), RiskState::Breached);
    acct.on_fill(Side::SELL, T(100.0), 100, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(1000), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 100.05), RiskState::Breached);
}

TEST(RiskManager, test_breached_recovery) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.cooldown_seconds = 5.0;
    cfg.max_stale_data_ms = 100000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md1 = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md1, 100.05), RiskState::Breached);
    acct.on_fill(Side::SELL, T(100.0), 100, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(6000), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 100.05), RiskState::Normal);
}

TEST(RiskManager, test_breached_no_recovery_if_warning) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    cfg.cooldown_seconds = 5.0;
    cfg.max_stale_data_ms = 100000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md1 = make_md(100.0, 100.10, base_time());
    EXPECT_EQ(rm.evaluate(acct, md1, 100.05), RiskState::Breached);
    acct.on_fill(Side::SELL, T(100.0), 15, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(6000), 2);
    EXPECT_EQ(rm.evaluate(acct, md2, 100.05), RiskState::Breached);
}

// ============================================================
// Kill-switch tests (4)
// ============================================================

TEST(RiskManager, test_kill_switch_engage) {
    RiskConfig cfg;
    RiskManager rm(cfg);
    rm.engage_kill_switch();
    EXPECT_EQ(rm.current_state(), RiskState::KillSwitch);
    EXPECT_FALSE(rm.is_quoting_allowed());
}

TEST(RiskManager, test_kill_switch_evaluate_cannot_exit) {
    RiskConfig cfg;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    rm.engage_kill_switch();
    auto md = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md, 100.05);
    EXPECT_EQ(rm.current_state(), RiskState::KillSwitch);
}

TEST(RiskManager, test_kill_switch_reset_safe) {
    RiskConfig cfg;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md, 100.05);
    rm.engage_kill_switch();
    rm.reset_kill_switch();
    EXPECT_EQ(rm.current_state(), RiskState::Normal);
    EXPECT_TRUE(rm.is_quoting_allowed());
}

TEST(RiskManager, test_kill_switch_reset_unsafe) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 100, true);
    auto md = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md, 100.05);
    rm.engage_kill_switch();
    EXPECT_EQ(rm.current_state(), RiskState::KillSwitch);
    rm.reset_kill_switch();
    EXPECT_EQ(rm.current_state(), RiskState::Breached);
    EXPECT_FALSE(rm.is_quoting_allowed());
}

// ============================================================
// Integration (1)
// ============================================================

TEST(RiskManager, test_is_quoting_allowed_integration) {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    RiskManager rm(cfg);
    Accounting acct(100000.0);

    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    EXPECT_EQ(rm.current_state(), RiskState::Normal);
    EXPECT_TRUE(rm.is_quoting_allowed());

    acct.on_fill(Side::BUY, T(100.0), 85, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(100), 2);
    rm.evaluate(acct, md2, 100.05);
    EXPECT_EQ(rm.current_state(), RiskState::Warning);
    EXPECT_TRUE(rm.is_quoting_allowed());

    acct.on_fill(Side::BUY, T(100.0), 20, true);
    auto md3 = make_md(100.0, 100.10, offset_ms(200), 3);
    rm.evaluate(acct, md3, 100.05);
    EXPECT_EQ(rm.current_state(), RiskState::Breached);
    EXPECT_FALSE(rm.is_quoting_allowed());

    rm.engage_kill_switch();
    EXPECT_EQ(rm.current_state(), RiskState::KillSwitch);
    EXPECT_FALSE(rm.is_quoting_allowed());
}

}  // namespace
