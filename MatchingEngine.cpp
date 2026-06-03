#include "MatchingEngine.h"

#include <algorithm>
#include <iterator>
#include <utility>

void MatchingEngine::Level::push_back(OrderNode* n) {
    n->prev = tail;
    n->next = nullptr;
    if (tail) tail->next = n;
    else      head       = n;
    tail = n;
    total_qty += n->leaves_qty;
}

void MatchingEngine::Level::unlink(OrderNode* n) {
    if (n->prev) n->prev->next = n->next;
    else         head          = n->next;
    if (n->next) n->next->prev = n->prev;
    else         tail          = n->prev;
    total_qty -= n->leaves_qty;
    n->prev = n->next = nullptr;
}

MatchingEngine::MatchingEngine() = default;

MatchingEngine::~MatchingEngine() = default;
// In-flight OrderNodes live in chunks_ which the unique_ptr vector frees
// in bulk. OrderNode is trivially destructible, so no per-node teardown
// is needed.

MatchingEngine::OrderNode* MatchingEngine::pool_alloc() {
    if (free_head_) {
        OrderNode* n = free_head_;
        free_head_ = n->next;
        return n;
    }
    if (chunks_.empty() || bump_pos_ == kChunkNodes) {
        chunks_.push_back(std::make_unique<Chunk>());
        bump_pos_ = 0;
    }
    return &chunks_.back()->nodes[bump_pos_++];
}

void MatchingEngine::pool_dealloc(OrderNode* n) {
    n->next = free_head_;
    free_head_ = n;
}

bool MatchingEngine::would_cross(Side incoming_side, Ticks price) const {
    if (incoming_side == Side::BUY) {
        return !asks_.empty() && price >= asks_.begin()->first;
    }
    return !bids_.empty() && price <= bids_.begin()->first;
}

void MatchingEngine::rest_node(OrderNode* n) {
    if (n->side == Side::BUY) bids_[n->price].push_back(n);
    else                      asks_[n->price].push_back(n);
    lookup_[n->order_id] = n;
}

void MatchingEngine::unlink_node_from_book(OrderNode* n) {
    if (n->side == Side::BUY) {
        auto it = bids_.find(n->price);
        it->second.unlink(n);
        if (it->second.empty()) bids_.erase(it);
    } else {
        auto it = asks_.find(n->price);
        it->second.unlink(n);
        if (it->second.empty()) asks_.erase(it);
    }
}

int MatchingEngine::match_against_book(Side aggressor_side, Ticks limit_price,
                                       int qty, uint64_t aggressor_trade_id,
                                       std::chrono::system_clock::time_point ts,
                                       std::vector<FillEvent>& fills) {
    int remaining = qty;

    // Per-side dispatch: BidBook and AskBook differ in comparator type, so
    // we duplicate the loop body. The semantics are identical — walk
    // best-priced levels first, fill FIFO at each level, unlink filled
    // makers, erase empty levels.
    if (aggressor_side == Side::BUY) {
        auto it = asks_.begin();
        while (remaining > 0 && it != asks_.end() && it->first <= limit_price) {
            Level& lvl = it->second;
            while (remaining > 0 && lvl.head) {
                OrderNode* head = lvl.head;
                int fill_qty    = std::min(remaining, head->leaves_qty);
                head->leaves_qty -= fill_qty;
                head->updated_at  = ts;
                lvl.total_qty    -= fill_qty;
                remaining        -= fill_qty;

                head->status = (head->leaves_qty == 0)
                    ? OrderStatus::FILLED
                    : OrderStatus::PARTIALLY_FILLED;

                fills.push_back(FillEvent{
                    head->order_id,
                    aggressor_trade_id,
                    head->side,
                    head->price,
                    fill_qty,
                    head->leaves_qty,
                    ts,
                });

                if (head->leaves_qty == 0) {
                    // Already decremented lvl.total_qty above; unlink will
                    // see leaves_qty==0 and decrement by 0.
                    lvl.unlink(head);
                    lookup_.erase(head->order_id);
                    pool_dealloc(head);
                }
            }
            if (lvl.empty()) it = asks_.erase(it);
            else             ++it;
        }
    } else {
        auto it = bids_.begin();
        while (remaining > 0 && it != bids_.end() && it->first >= limit_price) {
            Level& lvl = it->second;
            while (remaining > 0 && lvl.head) {
                OrderNode* head = lvl.head;
                int fill_qty    = std::min(remaining, head->leaves_qty);
                head->leaves_qty -= fill_qty;
                head->updated_at  = ts;
                lvl.total_qty    -= fill_qty;
                remaining        -= fill_qty;

                head->status = (head->leaves_qty == 0)
                    ? OrderStatus::FILLED
                    : OrderStatus::PARTIALLY_FILLED;

                fills.push_back(FillEvent{
                    head->order_id,
                    aggressor_trade_id,
                    head->side,
                    head->price,
                    fill_qty,
                    head->leaves_qty,
                    ts,
                });

                if (head->leaves_qty == 0) {
                    lvl.unlink(head);
                    lookup_.erase(head->order_id);
                    pool_dealloc(head);
                }
            }
            if (lvl.empty()) it = bids_.erase(it);
            else             ++it;
        }
    }

    return remaining;
}

SubmitResult MatchingEngine::add_order(Order order, OrderType type,
                                       uint64_t aggressor_trade_id) {
    SubmitResult result{OrderStatus::REJECTED, {}};

    if (order.leaves_qty <= 0 || order.price <= 0) {
        return result;
    }

    const bool marketable = would_cross(order.side, order.price);

    if (type == OrderType::POST_ONLY) {
        if (marketable)                      return result;  // REJECTED
        if (lookup_.count(order.order_id))   return result;  // duplicate id

        OrderNode* n = pool_alloc();
        *n = OrderNode{
            order.order_id, order.side, type, order.price,
            order.original_qty, order.leaves_qty,
            OrderStatus::ACKNOWLEDGED,
            order.created_at, order.created_at,
            nullptr, nullptr,
        };
        rest_node(n);
        result.status = OrderStatus::ACKNOWLEDGED;
        return result;
    }

    // LIMIT or IOC: match what's marketable, then deal with residual.
    int remaining = match_against_book(order.side, order.price,
                                       order.leaves_qty, aggressor_trade_id,
                                       order.created_at, result.fills);

    if (remaining == 0) {
        result.status = OrderStatus::FILLED;
        return result;
    }

    if (type == OrderType::IOC) {
        result.status = result.fills.empty()
            ? OrderStatus::CANCELED
            : OrderStatus::PARTIALLY_FILLED;
        return result;
    }

    // LIMIT residual rests.
    if (lookup_.count(order.order_id)) {
        // Duplicate id — fills are real, residual dropped.
        result.status = result.fills.empty()
            ? OrderStatus::REJECTED
            : OrderStatus::PARTIALLY_FILLED;
        return result;
    }

    OrderNode* n = pool_alloc();
    *n = OrderNode{
        order.order_id, order.side, type, order.price,
        order.original_qty, remaining,
        result.fills.empty() ? OrderStatus::ACKNOWLEDGED
                             : OrderStatus::PARTIALLY_FILLED,
        order.created_at, order.created_at,
        nullptr, nullptr,
    };
    rest_node(n);
    result.status = n->status;
    return result;
}

bool MatchingEngine::cancel_order(uint64_t id) {
    auto it = lookup_.find(id);
    if (it == lookup_.end()) return false;

    OrderNode* n = it->second;
    unlink_node_from_book(n);
    lookup_.erase(it);
    pool_dealloc(n);
    return true;
}

bool MatchingEngine::amend_order(uint64_t id, Ticks new_price, int new_qty,
                                  std::chrono::system_clock::time_point ts) {
    if (new_qty <= 0 || new_price <= 0)         return false;

    auto it = lookup_.find(id);
    if (it == lookup_.end())                    return false;

    OrderNode* n = it->second;

    // Cross check against opposite book. Since this order rests on its own
    // side, it doesn't influence the opposite-side best — `would_cross` is
    // a faithful test here. M4 simplification: cross-amend is uniformly
    // rejected. (Real LIMIT-resting orders would aggress on cross-amend;
    // that distinction lands in M9 when non-POST_ONLY rests exist.)
    if (would_cross(n->side, new_price))        return false;

    const bool price_changed = (new_price != n->price);
    const bool size_up       = (new_qty   >  n->leaves_qty);

    if (!price_changed && !size_up) {
        // In-place size reduction (or no-op). Queue position preserved.
        int delta = new_qty - n->leaves_qty;
        n->leaves_qty = new_qty;
        n->updated_at = ts;
        if (n->side == Side::BUY) bids_.find(n->price)->second.total_qty += delta;
        else                      asks_.find(n->price)->second.total_qty += delta;
        return true;
    }

    // Queue position is lost: unlink, mutate, re-rest at tail.
    unlink_node_from_book(n);
    n->price      = new_price;
    n->leaves_qty = new_qty;
    n->updated_at = ts;
    if (n->side == Side::BUY) bids_[n->price].push_back(n);
    else                      asks_[n->price].push_back(n);
    return true;
}

// ---- Inspection ----------------------------------------------------------

bool MatchingEngine::empty(Side s) const {
    return (s == Side::BUY) ? bids_.empty() : asks_.empty();
}

std::size_t MatchingEngine::num_levels(Side s) const {
    return (s == Side::BUY) ? bids_.size() : asks_.size();
}

std::size_t MatchingEngine::num_orders(Side s) const {
    std::size_t total = 0;
    auto count = [&](const auto& book) {
        for (auto& [_, lvl] : book) {
            for (auto* p = lvl.head; p; p = p->next) ++total;
        }
    };
    if (s == Side::BUY) count(bids_); else count(asks_);
    return total;
}

Ticks MatchingEngine::best_price(Side s) const {
    return (s == Side::BUY) ? bids_.begin()->first : asks_.begin()->first;
}

namespace {
template <class Book>
auto level_iter_at(const Book& b, std::size_t depth) {
    auto it = b.begin();
    std::advance(it, static_cast<typename Book::difference_type>(depth));
    return it;
}
} // namespace

std::size_t MatchingEngine::orders_at_depth(Side s, std::size_t depth) const {
    auto walk = [&](const auto& book) -> std::size_t {
        if (depth >= book.size()) return 0;
        auto it = level_iter_at(book, depth);
        std::size_t n = 0;
        for (auto* p = it->second.head; p; p = p->next) ++n;
        return n;
    };
    return (s == Side::BUY) ? walk(bids_) : walk(asks_);
}

int MatchingEngine::qty_at_depth(Side s, std::size_t depth) const {
    auto walk = [&](const auto& book) -> int {
        if (depth >= book.size()) return 0;
        return level_iter_at(book, depth)->second.total_qty;
    };
    return (s == Side::BUY) ? walk(bids_) : walk(asks_);
}

Ticks MatchingEngine::price_at_depth(Side s, std::size_t depth) const {
    auto walk = [&](const auto& book) -> Ticks {
        return level_iter_at(book, depth)->first;
    };
    return (s == Side::BUY) ? walk(bids_) : walk(asks_);
}

uint64_t MatchingEngine::order_id_at_queue_pos(Side s, std::size_t depth,
                                                std::size_t pos) const {
    auto walk = [&](const auto& book) -> uint64_t {
        if (depth >= book.size()) return 0;
        auto it = level_iter_at(book, depth);
        auto* p = it->second.head;
        for (std::size_t i = 0; i < pos && p; ++i) p = p->next;
        return p ? p->order_id : 0;
    };
    return (s == Side::BUY) ? walk(bids_) : walk(asks_);
}

bool MatchingEngine::contains(uint64_t id) const {
    return lookup_.find(id) != lookup_.end();
}

int MatchingEngine::leaves_qty_of(uint64_t id) const {
    auto it = lookup_.find(id);
    return it == lookup_.end() ? -1 : it->second->leaves_qty;
}

OrderStatus MatchingEngine::status_of(uint64_t id) const {
    auto it = lookup_.find(id);
    return it == lookup_.end() ? OrderStatus::CANCELED : it->second->status;
}
