#ifndef MARKET_DATA_EVENT_H
#define MARKET_DATA_EVENT_H

#include <string>
#include <chrono>
#include <vector>
#include "Order.h" // Include Order struct

struct MarketDataEvent {
    std::string instrument;
    double bid;
    double ask;
    int bid_size;
    int ask_size;
    std::vector<Order> bid_levels; // Use Order type
    std::vector<Order> ask_levels; // Use Order type
    std::chrono::system_clock::time_point timestamp;
};

#endif // MARKET_DATA_EVENT_H