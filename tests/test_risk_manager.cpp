#include <cassert>
#include <cmath>
#include <iostream>
#include <chrono>
#include "include/RiskManager.h"

namespace {

constexpr double EPS = 1e-6;

bool near(double a, double b) {
    return std::abs(a - b) < EPS;
}

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
    md.best_bid_price = bid;
    md.best_ask_price = ask;
    md.best_bid_size = 100;
    md.best_ask_size = 100;
    md.bid_levels.emplace_back(bid, 100, "B1", ts);
    md.ask_levels.emplace_back(ask, 100, "A1", ts);
    md.timestamp = ts;
    md.sequence_number = seq;
    return md;
}

// ============================================================
// Individual rule tests (11)
// ============================================================

// 1. max_net_position: normal
void test_max_net_position_normal() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md = make_md(100.0, 100.10, base_time());
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Normal);
    assert(rm.is_quoting_allowed());
    std::cout << "PASS: test_max_net_position_normal\n";
}

// 2. max_net_position: warning (80%)
void test_max_net_position_warning() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Buy 80 shares to hit 80% of limit
    for (int i = 0; i < 80; ++i)
        acct.on_fill(Side::BUY, 100.0, 1, true);
    auto md = make_md(100.0, 100.10, base_time());
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Warning);
    assert(rm.is_quoting_allowed());
    std::cout << "PASS: test_max_net_position_warning\n";
}

// 3. max_net_position: breached
void test_max_net_position_breached() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 100, true);
    auto md = make_md(100.0, 100.10, base_time());
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Breached);
    assert(!rm.is_quoting_allowed());
    std::cout << "PASS: test_max_net_position_breached\n";
}

// 4. max_notional_exposure: breached
void test_max_notional_exposure_breached() {
    RiskConfig cfg;
    cfg.max_notional_exposure = 5000.0;
    cfg.max_net_position = 10000; // high so it doesn't trigger
    RiskManager rm(cfg);
    Accounting acct(1000000.0);
    acct.on_fill(Side::BUY, 100.0, 100, true); // 10000 notional > 5000 limit
    auto md = make_md(100.0, 100.10, base_time());
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Breached);
    std::cout << "PASS: test_max_notional_exposure_breached\n";
}

// 5. max_drawdown: breached
void test_max_drawdown_breached() {
    RiskConfig cfg;
    cfg.max_drawdown = 100.0;
    cfg.max_net_position = 100000;
    cfg.max_notional_exposure = 1e9;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // First evaluate to set HWM at 0
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    // Create a loss > 100 drawdown: buy high, mark low
    acct.on_fill(Side::BUY, 100.0, 10, true); // position=10
    acct.mark_to_market(89.0); // unrealized = (89-100)*10 = -110, net_pnl ~ -110
    auto md2 = make_md(88.95, 89.05, offset_ms(100), 2);
    RiskState state = rm.evaluate(acct, md2, 89.0);
    assert(state == RiskState::Breached);
    assert(rm.current_drawdown() > 100.0);
    std::cout << "PASS: test_max_drawdown_breached\n";
}

// 6. max_drawdown: HWM tracks upward
void test_max_drawdown_hwm_tracks() {
    RiskConfig cfg;
    cfg.max_drawdown = 10000.0;
    cfg.max_net_position = 100000;
    cfg.max_notional_exposure = 1e9;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // net_pnl starts at 0 -> HWM=0
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    assert(near(rm.high_water_mark(), 0.0));
    // Create profit: buy at 100, mark at 110 -> unrealized=+100
    acct.on_fill(Side::BUY, 100.0, 10, true);
    acct.mark_to_market(110.0);
    auto md2 = make_md(109.95, 110.05, offset_ms(100), 2);
    rm.evaluate(acct, md2, 110.0);
    assert(rm.high_water_mark() > 0.0);
    double hwm = rm.high_water_mark();
    // Mark back down slightly — HWM should NOT decrease
    acct.mark_to_market(105.0);
    auto md3 = make_md(104.95, 105.05, offset_ms(200), 3);
    rm.evaluate(acct, md3, 105.0);
    assert(rm.high_water_mark() == hwm);
    std::cout << "PASS: test_max_drawdown_hwm_tracks\n";
}

// 7. max_quote_rate: breached
void test_max_quote_rate_breached() {
    RiskConfig cfg;
    cfg.max_quotes_per_second = 5.0;
    cfg.rate_window_seconds = 1.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto ts = base_time();
    // Record 5 quotes in the same window -> rate = 5/1 = 5 -> ratio = 1.0 -> breached
    for (int i = 0; i < 5; ++i)
        rm.record_quote(ts);
    auto md = make_md(100.0, 100.10, ts);
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Breached);
    std::cout << "PASS: test_max_quote_rate_breached\n";
}

// 8. max_cancel_rate: breached
void test_max_cancel_rate_breached() {
    RiskConfig cfg;
    cfg.max_cancels_per_second = 5.0;
    cfg.rate_window_seconds = 1.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto ts = base_time();
    for (int i = 0; i < 5; ++i)
        rm.record_cancel(ts);
    auto md = make_md(100.0, 100.10, ts);
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Breached);
    std::cout << "PASS: test_max_cancel_rate_breached\n";
}

// 9. stale_market_data: breached
void test_stale_market_data_breached() {
    RiskConfig cfg;
    cfg.max_stale_data_ms = 1000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // First tick — should be OK
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    // Second tick 2000ms later — stale
    auto md2 = make_md(100.0, 100.10, offset_ms(2000), 2);
    RiskState state = rm.evaluate(acct, md2, 100.05);
    assert(state == RiskState::Breached);
    std::cout << "PASS: test_stale_market_data_breached\n";
}

// 10. stale_market_data: first tick OK
void test_stale_market_data_first_tick() {
    RiskConfig cfg;
    cfg.max_stale_data_ms = 100.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md = make_md(100.0, 100.10, base_time());
    RiskState state = rm.evaluate(acct, md, 100.05);
    assert(state == RiskState::Normal);
    std::cout << "PASS: test_stale_market_data_first_tick\n";
}

// 11. max_quote_spread: breached
void test_max_quote_spread_breached() {
    RiskConfig cfg;
    cfg.max_quote_spread = 0.10;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Spread = 0.20 > 0.10 limit
    auto md = make_md(100.0, 100.20, base_time());
    RiskState state = rm.evaluate(acct, md, 100.10);
    assert(state == RiskState::Breached);
    std::cout << "PASS: test_max_quote_spread_breached\n";
}

// ============================================================
// State machine tests (5)
// ============================================================

// 12. normal → warning transition
void test_state_normal_to_warning() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Start normal
    auto md1 = make_md(100.0, 100.10, base_time());
    assert(rm.evaluate(acct, md1, 100.05) == RiskState::Normal);
    // Add position to 85% -> warning
    acct.on_fill(Side::BUY, 100.0, 85, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(100), 2);
    assert(rm.evaluate(acct, md2, 100.05) == RiskState::Warning);
    std::cout << "PASS: test_state_normal_to_warning\n";
}

// 13. normal → breached transition
void test_state_normal_to_breached() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    auto md1 = make_md(100.0, 100.10, base_time());
    assert(rm.evaluate(acct, md1, 100.05) == RiskState::Normal);
    acct.on_fill(Side::BUY, 100.0, 100, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(100), 2);
    assert(rm.evaluate(acct, md2, 100.05) == RiskState::Breached);
    std::cout << "PASS: test_state_normal_to_breached\n";
}

// 14. breached requires cooldown (limits recover but cooldown not elapsed)
void test_breached_requires_cooldown() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.cooldown_seconds = 5.0;
    cfg.max_stale_data_ms = 100000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Breach
    acct.on_fill(Side::BUY, 100.0, 100, true);
    auto md1 = make_md(100.0, 100.10, base_time());
    assert(rm.evaluate(acct, md1, 100.05) == RiskState::Breached);
    // Reduce position to normal
    acct.on_fill(Side::SELL, 100.0, 100, true);
    // Only 1 second later — cooldown not elapsed
    auto md2 = make_md(100.0, 100.10, offset_ms(1000), 2);
    assert(rm.evaluate(acct, md2, 100.05) == RiskState::Breached);
    std::cout << "PASS: test_breached_requires_cooldown\n";
}

// 15. breached recovery (cooldown elapsed + all normal)
void test_breached_recovery() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.cooldown_seconds = 5.0;
    cfg.max_stale_data_ms = 100000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Breach
    acct.on_fill(Side::BUY, 100.0, 100, true);
    auto md1 = make_md(100.0, 100.10, base_time());
    assert(rm.evaluate(acct, md1, 100.05) == RiskState::Breached);
    // Reduce position to normal
    acct.on_fill(Side::SELL, 100.0, 100, true);
    // 6 seconds later — cooldown elapsed
    auto md2 = make_md(100.0, 100.10, offset_ms(6000), 2);
    assert(rm.evaluate(acct, md2, 100.05) == RiskState::Normal);
    std::cout << "PASS: test_breached_recovery\n";
}

// 16. breached no recovery if still in warning zone
void test_breached_no_recovery_if_warning() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    cfg.cooldown_seconds = 5.0;
    cfg.max_stale_data_ms = 100000.0;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Breach
    acct.on_fill(Side::BUY, 100.0, 100, true);
    auto md1 = make_md(100.0, 100.10, base_time());
    assert(rm.evaluate(acct, md1, 100.05) == RiskState::Breached);
    // Reduce to warning zone (85 shares = 85% > 80% threshold)
    acct.on_fill(Side::SELL, 100.0, 15, true);
    // Cooldown elapsed
    auto md2 = make_md(100.0, 100.10, offset_ms(6000), 2);
    // Should stay Breached because not all-Normal
    assert(rm.evaluate(acct, md2, 100.05) == RiskState::Breached);
    std::cout << "PASS: test_breached_no_recovery_if_warning\n";
}

// ============================================================
// Kill-switch tests (4)
// ============================================================

// 17. engage stops quoting
void test_kill_switch_engage() {
    RiskConfig cfg;
    RiskManager rm(cfg);
    rm.engage_kill_switch();
    assert(rm.current_state() == RiskState::KillSwitch);
    assert(!rm.is_quoting_allowed());
    std::cout << "PASS: test_kill_switch_engage\n";
}

// 18. evaluate() cannot exit kill-switch
void test_kill_switch_evaluate_cannot_exit() {
    RiskConfig cfg;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    rm.engage_kill_switch();
    auto md = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md, 100.05);
    assert(rm.current_state() == RiskState::KillSwitch);
    std::cout << "PASS: test_kill_switch_evaluate_cannot_exit\n";
}

// 19. reset when safe → Normal
void test_kill_switch_reset_safe() {
    RiskConfig cfg;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    // Evaluate once to populate last_results (all normal)
    auto md = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md, 100.05);
    rm.engage_kill_switch();
    rm.reset_kill_switch();
    assert(rm.current_state() == RiskState::Normal);
    assert(rm.is_quoting_allowed());
    std::cout << "PASS: test_kill_switch_reset_safe\n";
}

// 20. reset when unsafe → Breached
void test_kill_switch_reset_unsafe() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    RiskManager rm(cfg);
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 100, true);
    auto md = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md, 100.05);
    // Now in Breached. Engage kill switch on top of it.
    rm.engage_kill_switch();
    assert(rm.current_state() == RiskState::KillSwitch);
    rm.reset_kill_switch();
    assert(rm.current_state() == RiskState::Breached);
    assert(!rm.is_quoting_allowed());
    std::cout << "PASS: test_kill_switch_reset_unsafe\n";
}

// ============================================================
// Integration (1)
// ============================================================

// 21. is_quoting_allowed: true for Normal/Warning, false for Breached/KillSwitch
void test_is_quoting_allowed_integration() {
    RiskConfig cfg;
    cfg.max_net_position = 100;
    cfg.warning_threshold_pct = 0.80;
    RiskManager rm(cfg);
    Accounting acct(100000.0);

    // Normal
    auto md1 = make_md(100.0, 100.10, base_time());
    rm.evaluate(acct, md1, 100.05);
    assert(rm.current_state() == RiskState::Normal);
    assert(rm.is_quoting_allowed() == true);

    // Warning
    acct.on_fill(Side::BUY, 100.0, 85, true);
    auto md2 = make_md(100.0, 100.10, offset_ms(100), 2);
    rm.evaluate(acct, md2, 100.05);
    assert(rm.current_state() == RiskState::Warning);
    assert(rm.is_quoting_allowed() == true);

    // Breached
    acct.on_fill(Side::BUY, 100.0, 20, true);
    auto md3 = make_md(100.0, 100.10, offset_ms(200), 3);
    rm.evaluate(acct, md3, 100.05);
    assert(rm.current_state() == RiskState::Breached);
    assert(rm.is_quoting_allowed() == false);

    // KillSwitch
    rm.engage_kill_switch();
    assert(rm.current_state() == RiskState::KillSwitch);
    assert(rm.is_quoting_allowed() == false);

    std::cout << "PASS: test_is_quoting_allowed_integration\n";
}

} // namespace

int main() {
    std::cout << "=== Risk Manager Tests ===\n";

    // Individual rule tests (11)
    test_max_net_position_normal();
    test_max_net_position_warning();
    test_max_net_position_breached();
    test_max_notional_exposure_breached();
    test_max_drawdown_breached();
    test_max_drawdown_hwm_tracks();
    test_max_quote_rate_breached();
    test_max_cancel_rate_breached();
    test_stale_market_data_breached();
    test_stale_market_data_first_tick();
    test_max_quote_spread_breached();

    // State machine tests (5)
    test_state_normal_to_warning();
    test_state_normal_to_breached();
    test_breached_requires_cooldown();
    test_breached_recovery();
    test_breached_no_recovery_if_warning();

    // Kill-switch tests (4)
    test_kill_switch_engage();
    test_kill_switch_evaluate_cannot_exit();
    test_kill_switch_reset_safe();
    test_kill_switch_reset_unsafe();

    // Integration (1)
    test_is_quoting_allowed_integration();

    std::cout << "\nAll 21 risk manager tests passed!\n";
    return 0;
}
