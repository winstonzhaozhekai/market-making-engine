#include "MarketSimulator.h"
#include <random>
#include <algorithm>

MarketSimulator::MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_)
    : instrument(std::move(instrument_)), mid_price(init_price_), spread(spread_), volatility(volatility_) {
    rng.seed(std::random_device{}());
    initialize_order_book();
}

void MarketSimulator::initialize_order_book() {
    // Generate initial L2 order book with multiple levels
    for (int i = 1; i <= 5; ++i) {
        double price_offset = i * spread / 2;
        order_book["BID"].emplace_back(mid_price - price_offset, rand() % 10 + 1);
        order_book["ASK"].emplace_back(mid_price + price_offset, rand() % 10 + 1);
    }
}

MarketDataEvent MarketSimulator::generate_event() {
    // Simulate price movement using geometric Brownian motion
    std::normal_distribution<> noise(0, volatility);
    mid_price += noise(rng);

    // Randomly modify the order book (e.g., add/remove orders)
    update_order_book();

    // Generate a market data event with L2 data
    MarketDataEvent event{
        instrument,
        order_book["BID"].front().price,
        order_book["ASK"].front().price,
        order_book["BID"].front().size,
        order_book["ASK"].front().size,
        order_book["BID"], // Expose full bid levels
        order_book["ASK"], // Expose full ask levels
        std::chrono::system_clock::now()
    };
    return event;
}

void MarketSimulator::update_order_book() {
    // Randomly add or remove orders
    for (const std::string& side : {"BID", "ASK"}) { // Use std::string for comparison
        if (rand() % 100 < 20) { // 20% chance to remove an order
            if (!order_book[side].empty()) {
                order_book[side].pop_back();
            }
        }
        if (rand() % 100 < 30) { // 30% chance to add a new order
            double price_offset = (side == "BID" ? -1 : 1) * (rand() % 5 + 1) * spread / 2;
            order_book[side].emplace_back(mid_price + price_offset, rand() % 10 + 1);
            std::sort(order_book[side].begin(), order_book[side].end(),
                [side](const Order& a, const Order& b) {
                    return (side == "BID") ? a.price > b.price : a.price < b.price;
                });
        }
    }
}