#include "MarketSimulator.h"
#include <random>
#include <algorithm>
#include <chrono>
#include <thread>

MarketSimulator::MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_, int latency_ms_)
    : instrument(std::move(instrument_)), mid_price(init_price_), spread(spread_), volatility(volatility_), latency_ms(latency_ms_) {
    rng.seed(std::random_device{}());
    initialize_order_book();
}

void MarketSimulator::initialize_order_book() {
    // Generate initial L2 order book with multiple levels
    for (int i = 1; i <= 5; ++i) {
        double price_offset = i * spread / 2;
        order_book["BID"].emplace_back(mid_price - price_offset, rand() % 10 + 1, generate_order_id(), current_time());
        order_book["ASK"].emplace_back(mid_price + price_offset, rand() % 10 + 1, generate_order_id(), current_time());
    }
}

MarketDataEvent MarketSimulator::generate_event() {
    // Simulate price movement using geometric Brownian motion
    std::normal_distribution<> noise(0, volatility);
    mid_price += noise(rng);

    // Randomly modify the order book (e.g., add/remove orders)
    update_order_book();

    // Simulate aggressor-initiated trades
    simulate_trade();

    auto event_creation_time = std::chrono::system_clock::now();

    if (latency_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
    }

    // Generate a market data event with L2 data
    MarketDataEvent event{
        instrument,
        order_book["BID"].front().price,
        order_book["ASK"].front().price,
        order_book["BID"].front().size,
        order_book["ASK"].front().size,
        order_book["BID"], // Expose full bid levels
        order_book["ASK"], // Expose full ask levels
        event_creation_time
    };
    return event;
}

void MarketSimulator::update_order_book() {
    // Randomly add or remove orders
    for (const std::string& side : {"BID", "ASK"}) {
        if (rand() % 100 < 20) { // 20% chance to remove an order
            if (!order_book[side].empty()) {
                order_book[side].pop_back();
            }
        }
        if (rand() % 100 < 30) { // 30% chance to add a new order
            double price_offset = (side == "BID" ? -1 : 1) * (rand() % 5 + 1) * spread / 2;
            order_book[side].emplace_back(mid_price + price_offset, rand() % 10 + 1, generate_order_id(), current_time());
            std::sort(order_book[side].begin(), order_book[side].end(),
                [side](const Order& a, const Order& b) {
                    return (side == "BID") ? a.price > b.price : a.price < b.price;
                });
        }
    }
}

void MarketSimulator::simulate_trade() {
    // Simulate a trade by selecting the top of the book
    if (!order_book["BID"].empty() && !order_book["ASK"].empty()) {
        std::string aggressor_side = (rand() % 2 == 0) ? "BUY" : "SELL";
        double trade_price = (aggressor_side == "BUY") ? order_book["ASK"].front().price : order_book["BID"].front().price;
        int trade_size = rand() % 5 + 1;

        // Adjust the order book based on the trade
        if (aggressor_side == "BUY") {
            if (order_book["ASK"].front().size <= trade_size) {
                order_book["ASK"].erase(order_book["ASK"].begin());
            } else {
                order_book["ASK"].front().size -= trade_size;
            }
        } else {
            if (order_book["BID"].front().size <= trade_size) {
                order_book["BID"].erase(order_book["BID"].begin());
            } else {
                order_book["BID"].front().size -= trade_size;
            }
        }

        // Log the trade
        trades.emplace_back(trade_price, trade_size, aggressor_side, current_time());
    }
}

std::string MarketSimulator::generate_order_id() {
    static int id_counter = 0;
    return "ORD" + std::to_string(++id_counter);
}

std::chrono::system_clock::time_point MarketSimulator::current_time() {
    return std::chrono::system_clock::now();
}