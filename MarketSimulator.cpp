#include "MarketSimulator.h"
#include "MarketDataEvent.h" // Include the correct header file
#include <iostream>
#include <random>
#include <algorithm>
#include <chrono>
#include <thread>

std::map<std::string, std::vector<OrderLevel>> order_book; // Correct type

MarketSimulator::MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_, int latency_ms_)
    : instrument(std::move(instrument_)), mid_price(init_price_), spread(spread_), volatility(volatility_), latency_ms(latency_ms_), sequence_number(0) { // Initialize sequence_number
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
    
    // Prevent negative prices
    mid_price = std::max(mid_price, 0.01);

    // Randomly modify the order book
    update_order_book();

    // Generate trades and partial fills
    std::vector<Trade> trades;
    std::vector<PartialFillEvent> partial_fills;
    simulate_trade_activity(trades, partial_fills);

    auto event_creation_time = std::chrono::system_clock::now();

    if (latency_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
    }

    // Create market data event
    MarketDataEvent event{
        instrument,
        order_book["BID"].empty() ? 0.0 : order_book["BID"].front().price,
        order_book["ASK"].empty() ? 0.0 : order_book["ASK"].front().price,
        order_book["BID"].empty() ? 0 : order_book["BID"].front().size,
        order_book["ASK"].empty() ? 0 : order_book["ASK"].front().size,
        order_book["BID"],
        order_book["ASK"],
        trades,
        partial_fills,
        event_creation_time,
        ++sequence_number
    };

    // Debugging output
    std::cout << "Generated MarketDataEvent: Sequence " << event.sequence_number 
              << ", Best Bid: " << event.best_bid_price 
              << ", Best Ask: " << event.best_ask_price << std::endl;

    return event;
}

void MarketSimulator::simulate_trade_activity(std::vector<Trade>& trades, std::vector<PartialFillEvent>& partial_fills) {
    static std::uniform_real_distribution<> prob_dist(0.0, 1.0);
    static std::uniform_int_distribution<> size_dist(1, 20);
    
    // 20% chance of trade activity
    if (prob_dist(rng) < 0.2) {
        std::string side = (prob_dist(rng) < 0.5) ? "BUY" : "SELL";
        auto& levels = (side == "BUY") ? order_book["ASK"] : order_book["BID"];
        
        if (!levels.empty()) {
            int trade_size = size_dist(rng);
            double trade_price = levels[0].price;
            
            trades.emplace_back(Trade{
                side,
                trade_price,
                trade_size,
                "TRADE_" + std::to_string(sequence_number),
                current_time()
            });
            
            // 40% chance of generating partial fill
            if (prob_dist(rng) < 0.4) {
                int filled_size = std::max(1, trade_size / 2);
                partial_fills.emplace_back(PartialFillEvent{
                    "ORDER_" + std::to_string(sequence_number),
                    trade_price,
                    filled_size,
                    trade_size - filled_size,
                    current_time()
                });
            }
        }
    }
}

void MarketSimulator::update_order_book() {
    // Randomly update order book levels
    static std::uniform_real_distribution<> change_dist(-0.001, 0.001);
    static std::uniform_int_distribution<> size_change_dist(-2, 2);
    
    for (auto& side : {"BID", "ASK"}) {
        for (auto& level : order_book[side]) {
            level.price += change_dist(rng);
            level.size = std::max(1, level.size + size_change_dist(rng));
        }
        
        // Sort order book
        std::sort(order_book[side].begin(), order_book[side].end(),
                  [](const OrderLevel& a, const OrderLevel& b) { return a.price > b.price; });
    }
}

std::string MarketSimulator::generate_order_id() {
    static int counter = 0;
    return "SIM_" + std::to_string(++counter);
}

std::chrono::system_clock::time_point MarketSimulator::current_time() {
    return std::chrono::system_clock::now();
}