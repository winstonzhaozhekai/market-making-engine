#ifndef ORDER_H
#define ORDER_H

#include <cstdint>
#include <chrono>
#include "include/Instrument.h"

enum class Side { BUY, SELL };
enum class OrderStatus { NEW, ACKNOWLEDGED, PARTIALLY_FILLED, FILLED, CANCELED, REJECTED };

// Order type / TIF combo, mirroring what real venues actually expose.
//   LIMIT      — match what's marketable; rest residual at limit price.
//   POST_ONLY  — reject if marketable; otherwise rest. (Maker intent.)
//   IOC        — match what's marketable; cancel residual (no rest).
// Real exchanges encode this as a TIF + flag combo (NASDAQ POST, ARCA ALO,
// CME PostOnly, etc.). We collapse the relevant axis into one enum.
enum class OrderType { LIMIT, POST_ONLY, IOC };

struct Order {
    uint64_t order_id;
    Side side;
    Ticks price;
    int original_qty;
    int leaves_qty;
    OrderStatus status;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;

    Order(uint64_t id, Side s, Ticks p, int qty,
          std::chrono::system_clock::time_point ts)
        : order_id(id), side(s), price(p),
          original_qty(qty), leaves_qty(qty),
          status(OrderStatus::NEW), created_at(ts), updated_at(ts) {}
};

struct FillEvent {
    uint64_t order_id;
    uint64_t trade_id;
    Side side;
    Ticks price;
    int fill_qty;
    int leaves_qty;
    std::chrono::system_clock::time_point timestamp;
};

#endif // ORDER_H
