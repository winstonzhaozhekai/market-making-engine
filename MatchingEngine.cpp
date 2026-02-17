#include "MatchingEngine.h"
#include <algorithm>

OrderStatus MatchingEngine::add_order(Order order) {
    if (order.leaves_qty <= 0 || order.price <= 0.0) {
        order.status = OrderStatus::REJECTED;
        return OrderStatus::REJECTED;
    }

    order.status = OrderStatus::ACKNOWLEDGED;

    if (order.side == Side::BUY) {
        insert_bid(std::move(order));
    } else {
        insert_ask(std::move(order));
    }

    return OrderStatus::ACKNOWLEDGED;
}

void MatchingEngine::insert_bid(Order order) {
    auto it = std::lower_bound(bid_book.begin(), bid_book.end(), order,
        [](const Order& existing, const Order& incoming) {
            if (existing.price != incoming.price)
                return existing.price > incoming.price; // descending by price
            return existing.created_at < incoming.created_at; // ascending by time (earlier first)
        });
    bid_book.insert(it, std::move(order));
}

void MatchingEngine::insert_ask(Order order) {
    auto it = std::lower_bound(ask_book.begin(), ask_book.end(), order,
        [](const Order& existing, const Order& incoming) {
            if (existing.price != incoming.price)
                return existing.price < incoming.price; // ascending by price
            return existing.created_at < incoming.created_at; // ascending by time (earlier first)
        });
    ask_book.insert(it, std::move(order));
}

bool MatchingEngine::cancel_order(uint64_t order_id) {
    for (auto it = bid_book.begin(); it != bid_book.end(); ++it) {
        if (it->order_id == order_id) {
            it->status = OrderStatus::CANCELED;
            bid_book.erase(it);
            return true;
        }
    }
    for (auto it = ask_book.begin(); it != ask_book.end(); ++it) {
        if (it->order_id == order_id) {
            it->status = OrderStatus::CANCELED;
            ask_book.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<FillEvent> MatchingEngine::match_incoming_order(
    Side aggressor_side, double price, int qty,
    uint64_t trade_id,
    std::chrono::system_clock::time_point timestamp)
{
    std::vector<FillEvent> fills;
    int remaining = qty;

    // Aggressor BUY hits resting ASKs; aggressor SELL hits resting BIDs
    auto& passive_book = (aggressor_side == Side::BUY) ? ask_book : bid_book;

    auto it = passive_book.begin();
    while (it != passive_book.end() && remaining > 0) {
        // Check price compatibility
        if (aggressor_side == Side::BUY && it->price > price) break;
        if (aggressor_side == Side::SELL && it->price < price) break;

        int fill_qty = std::min(remaining, it->leaves_qty);
        it->leaves_qty -= fill_qty;
        it->updated_at = timestamp;
        remaining -= fill_qty;

        if (it->leaves_qty == 0) {
            it->status = OrderStatus::FILLED;
        } else {
            it->status = OrderStatus::PARTIALLY_FILLED;
        }

        fills.push_back(FillEvent{
            it->order_id,
            trade_id,
            it->side,
            it->price,
            fill_qty,
            it->leaves_qty,
            timestamp
        });

        if (it->leaves_qty == 0) {
            it = passive_book.erase(it);
        } else {
            ++it;
        }
    }

    return fills;
}
