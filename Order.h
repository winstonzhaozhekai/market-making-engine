#ifndef ORDER_H
#define ORDER_H

#include <string>
#include <chrono>

enum class Side { BUY, SELL };
enum class OrderStatus { NEW, ACKNOWLEDGED, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED };

struct Order {
    std::string order_id;
    Side side;
    double price;
    int original_qty;
    int leaves_qty;       // remaining unfilled quantity
    OrderStatus status;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;

    // Full constructor
    Order(std::string id, Side s, double p, int qty,
          std::chrono::system_clock::time_point ts)
        : order_id(std::move(id)), side(s), price(p),
          original_qty(qty), leaves_qty(qty),
          status(OrderStatus::NEW), created_at(ts), updated_at(ts) {}

    // Legacy constructor for compatibility with existing OrderLevel-style usage
    Order(double price_, int size_, std::string order_id_,
          std::chrono::system_clock::time_point timestamp_)
        : order_id(std::move(order_id_)), side(Side::BUY), price(price_),
          original_qty(size_), leaves_qty(size_),
          status(OrderStatus::NEW), created_at(timestamp_), updated_at(timestamp_) {}
};

struct FillEvent {
    std::string order_id;
    std::string trade_id;
    Side side;
    double price;
    int fill_qty;
    int leaves_qty;
    std::chrono::system_clock::time_point timestamp;
};

#endif // ORDER_H
