#include "MarketSimulator.h"
#include <random>

MarketSimulator::MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_)
    : instrument(std::move(instrument_)), mid_price(init_price_), spread(spread_), volatility(volatility_) {
    rng.seed(std::random_device{}());
}

MarketDataEvent MarketSimulator::generate_event() {
    std::normal_distribution<> noise(0, volatility);
    mid_price += noise(rng);
    MarketDataEvent event{
        instrument,
        mid_price - spread / 2,
        mid_price + spread / 2,
        rand() % 10 + 1,
        rand() % 10 + 1,
        std::chrono::system_clock::now()
    };
    return event;
}