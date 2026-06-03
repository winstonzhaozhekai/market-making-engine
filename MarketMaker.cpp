#include "MarketMaker.h"
#include "include/HeuristicStrategy.h"

namespace {
std::unique_ptr<Strategy> default_strategy() {
    return std::make_unique<HeuristicStrategy>();
}
std::unique_ptr<Logger> default_logger() {
    return std::make_unique<NullLogger>();
}
}

MarketMaker::MarketMaker(Instrument instrument)
    : strategy_(default_strategy()),
      logger_(default_logger()),
      impl_(instrument, RiskConfig{}, strategy_.get(), logger_.get()) {}

MarketMaker::MarketMaker(Instrument instrument, const RiskConfig& cfg)
    : strategy_(default_strategy()),
      logger_(default_logger()),
      impl_(instrument, cfg, strategy_.get(), logger_.get()) {}

MarketMaker::MarketMaker(Instrument instrument, const RiskConfig& cfg,
                         std::unique_ptr<Strategy> strategy)
    : strategy_(std::move(strategy)),
      logger_(default_logger()),
      impl_(instrument, cfg, strategy_.get(), logger_.get()) {}

MarketMaker::MarketMaker(Instrument instrument, const RiskConfig& cfg,
                         std::unique_ptr<Strategy> strategy,
                         std::unique_ptr<Logger> logger)
    : strategy_(std::move(strategy)),
      logger_(logger ? std::move(logger) : default_logger()),
      impl_(instrument, cfg, strategy_.get(), logger_.get()) {}
