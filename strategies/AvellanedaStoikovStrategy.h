#ifndef AVELLANEDA_STOIKOV_STRATEGY_H
#define AVELLANEDA_STOIKOV_STRATEGY_H

#include "../include/Strategy.h"
#include "../include/RollingEstimators.h"

struct AvellanedaStoikovConfig {
    double gamma = 0.1;           // risk aversion
    double kappa = 1.5;           // fill-rate parameter
    double T = 1.0;               // time horizon
    double min_spread_bps = 5.0;  // floor in basis points
    double max_spread_bps = 200.0;// ceiling in basis points
    double ofi_spread_factor = 0.5;
    int base_size = 5;
    double size_inventory_scale = 1.0;
    double toxic_ofi_threshold = 0.7;
    bool pull_on_toxic = false;
    size_t vol_window = 100;
    size_t ofi_window = 50;
};

class AvellanedaStoikovStrategy : public Strategy {
public:
    explicit AvellanedaStoikovStrategy(const AvellanedaStoikovConfig& cfg = AvellanedaStoikovConfig{});

    QuoteDecision compute_quotes(const StrategySnapshot& snapshot) override;
    const char* name() const override;

    const AvellanedaStoikovConfig& config() const { return config_; }
    double last_sigma() const { return vol_estimator_.sigma(); }
    double last_ofi() const { return ofi_estimator_.normalized_ofi(); }

private:
    AvellanedaStoikovConfig config_;
    RollingVolatility vol_estimator_;
    RollingOFI ofi_estimator_;
};

#endif // AVELLANEDA_STOIKOV_STRATEGY_H
