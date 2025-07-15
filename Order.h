#ifndef ORDER_H
#define ORDER_H

#include <string>
#include <chrono>

struct Order {
    double price;
    int size;
    std::string order_id;
    std::chrono::system_clock::time_point timestamp;

    // Constructor to match usage in MarketSimulator
    Order(double price_, int size_, std::string order_id_, std::chrono::system_clock::time_point timestamp_)
        : price(price_), size(size_), order_id(std::move(order_id_)), timestamp(timestamp_) {}
};

#endif // ORDER_H