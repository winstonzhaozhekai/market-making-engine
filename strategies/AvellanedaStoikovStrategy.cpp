#include "AvellanedaStoikovStrategy.h"
#include <cmath>
#include <algorithm>

AvellanedaStoikovStrategy::AvellanedaStoikovStrategy(const AvellanedaStoikovConfig& cfg)
    : config_(cfg),
      vol_estimator_(cfg.vol_window),
      ofi_estimator_(cfg.ofi_window) {}

QuoteDecision AvellanedaStoikovStrategy::compute_quotes(const StrategySnapshot& snap) {
    // Feed estimators
    vol_estimator_.on_mid(snap.mid_price);
    ofi_estimator_.on_trades(snap.trades);

    double sigma = vol_estimator_.sigma();
    double ofi = ofi_estimator_.normalized_ofi();
    double q = static_cast<double>(snap.position);
    double q_max = static_cast<double>(snap.max_position);
    double gamma = config_.gamma;
    double kappa = config_.kappa;
    double T = config_.T;

    // Reservation price: r = mid - q * gamma * sigma^2 * T
    double sigma2 = sigma * sigma;
    double reservation = snap.mid_price - q * gamma * sigma2 * T;

    // Optimal spread: delta = gamma*sigma^2*T + (2/gamma)*ln(1 + gamma/kappa)
    double optimal_spread = gamma * sigma2 * T + (2.0 / gamma) * std::log(1.0 + gamma / kappa);

    // OFI widening
    optimal_spread *= (1.0 + config_.ofi_spread_factor * std::abs(ofi));

    // Clamp spread to [min_bps, max_bps] of mid
    double min_spread = config_.min_spread_bps * snap.mid_price / 10000.0;
    double max_spread = config_.max_spread_bps * snap.mid_price / 10000.0;
    optimal_spread = std::max(min_spread, std::min(optimal_spread, max_spread));

    // Quote prices
    double bid_price = reservation - optimal_spread / 2.0;
    double ask_price = reservation + optimal_spread / 2.0;

    // Adverse selection: if |ofi| > toxic_threshold and pull_on_toxic, don't quote
    if (std::abs(ofi) > config_.toxic_ofi_threshold && config_.pull_on_toxic) {
        QuoteDecision decision;
        decision.should_quote = false;
        return decision;
    }

    // Asymmetric sizing
    double inv_ratio = (q_max > 0.0) ? q / q_max : 0.0;
    inv_ratio = std::max(-1.0, std::min(1.0, inv_ratio));

    double bid_size_d = config_.base_size * (1.0 - inv_ratio * config_.size_inventory_scale);
    double ask_size_d = config_.base_size * (1.0 + inv_ratio * config_.size_inventory_scale);

    int bid_size = std::max(1, static_cast<int>(bid_size_d));
    int ask_size = std::max(1, static_cast<int>(ask_size_d));

    QuoteDecision decision;
    decision.bid_price = bid_price;
    decision.ask_price = ask_price;
    decision.bid_size = bid_size;
    decision.ask_size = ask_size;
    decision.should_quote = true;
    return decision;
}

const char* AvellanedaStoikovStrategy::name() const {
    return "avellaneda-stoikov";
}
