#ifndef RISKMANAGER_H
#define RISKMANAGER_H

#include "Accounting.h"
#include "../MarketDataEvent.h"
#include <vector>
#include <deque>
#include <chrono>

enum class RiskState { Normal, Warning, Breached, KillSwitch };

enum class RiskRuleId {
    MaxNetPosition,
    MaxNotionalExposure,
    MaxDrawdown,
    MaxQuoteRate,
    MaxCancelRate,
    StaleMarketData,
    MaxQuoteSpread
};

struct RiskRuleResult {
    RiskRuleId rule_id;
    RiskState level;
    double current_value;
    double limit_value;
    const char* tag;  // static label instead of dynamic string
};

struct RiskConfig {
    int max_net_position = 1000;
    double max_notional_exposure = 500000.0;
    double max_drawdown = 10000.0;
    double max_quotes_per_second = 50.0;
    double max_cancels_per_second = 50.0;
    double rate_window_seconds = 1.0;
    double max_stale_data_ms = 5000.0;
    double warning_threshold_pct = 0.80;
    double cooldown_seconds = 5.0;
    double max_quote_spread = 0.5;
    int min_quote_size = 1;
    int max_quote_size = 100;
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& cfg = RiskConfig{});

    RiskState evaluate(const Accounting& acct, const MarketDataEvent& md, double mark_price);

    void engage_kill_switch();
    void reset_kill_switch();

    void record_quote(std::chrono::system_clock::time_point ts);
    void record_cancel(std::chrono::system_clock::time_point ts);

    bool is_quoting_allowed() const;
    RiskState current_state() const;
    const std::vector<RiskRuleResult>& last_results() const;
    double current_drawdown() const;
    double high_water_mark() const;
    const RiskConfig& config() const;

private:
    RiskConfig config_;
    RiskState state_ = RiskState::Normal;
    std::vector<RiskRuleResult> last_results_;

    double high_water_mark_ = 0.0;
    double drawdown_ = 0.0;
    bool hwm_initialized_ = false;

    std::deque<std::chrono::system_clock::time_point> quote_timestamps_;
    std::deque<std::chrono::system_clock::time_point> cancel_timestamps_;

    std::chrono::system_clock::time_point breach_timestamp_;
    bool breach_timestamp_set_ = false;

    std::chrono::system_clock::time_point last_md_timestamp_;
    bool last_md_timestamp_set_ = false;

    RiskState classify(double ratio) const;

    RiskRuleResult eval_max_net_position(const Accounting& acct);
    RiskRuleResult eval_max_notional_exposure(const Accounting& acct, double mark_price);
    RiskRuleResult eval_max_drawdown(const Accounting& acct);
    RiskRuleResult eval_max_quote_rate(std::chrono::system_clock::time_point now);
    RiskRuleResult eval_max_cancel_rate(std::chrono::system_clock::time_point now);
    RiskRuleResult eval_stale_market_data(std::chrono::system_clock::time_point md_ts);
    RiskRuleResult eval_max_quote_spread(const MarketDataEvent& md);
};

#endif // RISKMANAGER_H
