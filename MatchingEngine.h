#ifndef MATCHING_ENGINE_H
#define MATCHING_ENGINE_H

#include "Order.h"
#include <cstdint>
#include <vector>

class MatchingEngine {
public:
    OrderStatus add_order(Order order);
    bool cancel_order(uint64_t order_id);
    std::vector<FillEvent> match_incoming_order(Side aggressor_side, double price, int qty,
                                                 uint64_t trade_id,
                                                 std::chrono::system_clock::time_point timestamp);

    const std::vector<Order>& get_bids() const { return bid_book; }
    const std::vector<Order>& get_asks() const { return ask_book; }

private:
    std::vector<Order> bid_book; // sorted descending by price, then ascending by time
    std::vector<Order> ask_book; // sorted ascending by price, then ascending by time

    void insert_bid(Order order);
    void insert_ask(Order order);
};

#endif // MATCHING_ENGINE_H
