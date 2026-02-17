#include "include/RiskManager.h"
#include <cmath>
#include <algorithm>

RiskManager::RiskManager(const RiskConfig& cfg)
    : config_(cfg) {
    last_results_.reserve(7);
}

RiskState RiskManager::classify(double ratio) const {
    if (ratio >= 1.0) return RiskState::Breached;
    if (ratio >= config_.warning_threshold_pct) return RiskState::Warning;
    return RiskState::Normal;
}

RiskRuleResult RiskManager::eval_max_net_position(const Accounting& acct) {
    double current = std::abs(static_cast<double>(acct.position()));
    double limit = static_cast<double>(config_.max_net_position);
    double ratio = current / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::MaxNetPosition, level, current, limit, "net_position"};
}

RiskRuleResult RiskManager::eval_max_notional_exposure(const Accounting& acct, double mark_price) {
    double current = acct.gross_exposure(mark_price);
    double limit = config_.max_notional_exposure;
    double ratio = current / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::MaxNotionalExposure, level, current, limit, "gross_exposure"};
}

RiskRuleResult RiskManager::eval_max_drawdown(const Accounting& acct) {
    double pnl = acct.net_pnl();

    if (!hwm_initialized_) {
        high_water_mark_ = pnl;
        hwm_initialized_ = true;
    } else if (pnl > high_water_mark_) {
        high_water_mark_ = pnl;
    }

    drawdown_ = high_water_mark_ - pnl;
    double limit = config_.max_drawdown;
    double ratio = drawdown_ / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::MaxDrawdown, level, drawdown_, limit, "drawdown"};
}

RiskRuleResult RiskManager::eval_max_quote_rate(std::chrono::system_clock::time_point now) {
    auto window = std::chrono::duration<double>(config_.rate_window_seconds);
    auto cutoff = now - std::chrono::duration_cast<std::chrono::system_clock::duration>(window);

    while (!quote_timestamps_.empty() && quote_timestamps_.front() < cutoff) {
        quote_timestamps_.pop_front();
    }

    double current = static_cast<double>(quote_timestamps_.size()) / config_.rate_window_seconds;
    double limit = config_.max_quotes_per_second;
    double ratio = current / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::MaxQuoteRate, level, current, limit, "quote_rate"};
}

RiskRuleResult RiskManager::eval_max_cancel_rate(std::chrono::system_clock::time_point now) {
    auto window = std::chrono::duration<double>(config_.rate_window_seconds);
    auto cutoff = now - std::chrono::duration_cast<std::chrono::system_clock::duration>(window);

    while (!cancel_timestamps_.empty() && cancel_timestamps_.front() < cutoff) {
        cancel_timestamps_.pop_front();
    }

    double current = static_cast<double>(cancel_timestamps_.size()) / config_.rate_window_seconds;
    double limit = config_.max_cancels_per_second;
    double ratio = current / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::MaxCancelRate, level, current, limit, "cancel_rate"};
}

RiskRuleResult RiskManager::eval_stale_market_data(std::chrono::system_clock::time_point md_ts) {
    if (!last_md_timestamp_set_) {
        last_md_timestamp_ = md_ts;
        last_md_timestamp_set_ = true;
        return {RiskRuleId::StaleMarketData, RiskState::Normal, 0.0, config_.max_stale_data_ms, "first_tick"};
    }

    auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(md_ts - last_md_timestamp_);
    double current_ms = static_cast<double>(gap.count());
    last_md_timestamp_ = md_ts;

    double limit = config_.max_stale_data_ms;
    double ratio = current_ms / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::StaleMarketData, level, current_ms, limit, "stale_ms"};
}

RiskRuleResult RiskManager::eval_max_quote_spread(const MarketDataEvent& md) {
    double spread = md.best_ask_price - md.best_bid_price;
    double limit = config_.max_quote_spread;
    double ratio = spread / limit;
    RiskState level = classify(ratio);
    return {RiskRuleId::MaxQuoteSpread, level, spread, limit, "spread"};
}

RiskState RiskManager::evaluate(const Accounting& acct, const MarketDataEvent& md, double mark_price) {
    if (state_ == RiskState::KillSwitch) {
        return state_;
    }

    last_results_.clear();
    last_results_.push_back(eval_max_net_position(acct));
    last_results_.push_back(eval_max_notional_exposure(acct, mark_price));
    last_results_.push_back(eval_max_drawdown(acct));
    last_results_.push_back(eval_max_quote_rate(md.timestamp));
    last_results_.push_back(eval_max_cancel_rate(md.timestamp));
    last_results_.push_back(eval_stale_market_data(md.timestamp));
    last_results_.push_back(eval_max_quote_spread(md));

    // Aggregate: worst state across all rules
    RiskState worst = RiskState::Normal;
    for (const auto& r : last_results_) {
        if (static_cast<int>(r.level) > static_cast<int>(worst)) {
            worst = r.level;
        }
    }

    // State machine transitions
    switch (state_) {
        case RiskState::Normal:
        case RiskState::Warning:
            if (worst == RiskState::Breached) {
                state_ = RiskState::Breached;
                breach_timestamp_ = md.timestamp;
                breach_timestamp_set_ = true;
            } else {
                state_ = worst; // Normal or Warning
            }
            break;
        case RiskState::Breached: {
            // Can only recover if cooldown elapsed AND all rules Normal
            if (worst == RiskState::Normal && breach_timestamp_set_) {
                auto elapsed = std::chrono::duration<double>(md.timestamp - breach_timestamp_);
                if (elapsed.count() >= config_.cooldown_seconds) {
                    state_ = RiskState::Normal;
                }
                // else stays Breached (cooldown not elapsed)
            }
            // If worst is Warning or Breached, stays Breached
            break;
        }
        case RiskState::KillSwitch:
            // Only reset_kill_switch() can change this
            break;
    }

    return state_;
}

void RiskManager::engage_kill_switch() {
    state_ = RiskState::KillSwitch;
}

void RiskManager::reset_kill_switch() {
    if (state_ != RiskState::KillSwitch) return;

    // Check last results to determine if safe
    RiskState worst = RiskState::Normal;
    for (const auto& r : last_results_) {
        if (static_cast<int>(r.level) > static_cast<int>(worst)) {
            worst = r.level;
        }
    }

    if (worst == RiskState::Normal) {
        state_ = RiskState::Normal;
    } else {
        state_ = RiskState::Breached;
        // Don't set breach_timestamp here — will be set on next evaluate()
    }
}

void RiskManager::record_quote(std::chrono::system_clock::time_point ts) {
    quote_timestamps_.push_back(ts);
}

void RiskManager::record_cancel(std::chrono::system_clock::time_point ts) {
    cancel_timestamps_.push_back(ts);
}

bool RiskManager::is_quoting_allowed() const {
    return state_ == RiskState::Normal || state_ == RiskState::Warning;
}

RiskState RiskManager::current_state() const {
    return state_;
}

const std::vector<RiskRuleResult>& RiskManager::last_results() const {
    return last_results_;
}

double RiskManager::current_drawdown() const {
    return drawdown_;
}

double RiskManager::high_water_mark() const {
    return high_water_mark_;
}

const RiskConfig& RiskManager::config() const {
    return config_;
}
