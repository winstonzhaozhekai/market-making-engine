#ifndef MARKETDATAEVENT_H
#define MARKETDATAEVENT_H

#include <vector>
#include <cstdint>
#include <chrono>
#include "Order.h"

struct OrderLevel {
    double price;
    int size;
    uint64_t order_id;
    std::chrono::system_clock::time_point timestamp;

    // Constructor
    OrderLevel(double p, int s, uint64_t id, std::chrono::system_clock::time_point ts)
        : price(p), size(s), order_id(id), timestamp(ts) {}
};

struct Trade {
    Side aggressor_side;
    double price;
    int size;
    uint64_t trade_id;
    std::chrono::system_clock::time_point timestamp;
};

struct PartialFillEvent {
    uint64_t order_id;
    double price;
    int filled_size;
    int remaining_size;
    std::chrono::system_clock::time_point timestamp;
};

struct MarketDataEvent {
    std::string instrument;
    double best_bid_price;
    double best_ask_price;
    int best_bid_size;
    int best_ask_size;
    std::vector<OrderLevel> bid_levels;
    std::vector<OrderLevel> ask_levels;
    std::vector<Trade> trades;
    std::vector<PartialFillEvent> partial_fills;
    std::vector<FillEvent> mm_fills;
    std::chrono::system_clock::time_point timestamp;
    int64_t sequence_number;
};

#endif
