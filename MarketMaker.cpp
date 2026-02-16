#include "MarketMaker.h"
#include "MarketSimulator.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

MarketMaker::MarketMaker() {
    last_quote_time = std::chrono::system_clock::now();
}

void MarketMaker::on_market_data(const MarketDataEvent& md, MarketSimulator& simulator) {
    if (md.sequence_number != last_processed_sequence + 1 && last_processed_sequence != 0) {
        std::cout << "WARNING: Sequence gap detected. Missed "
                  << (md.sequence_number - last_processed_sequence - 1)
                  << " events\n";
    }
    last_processed_sequence = md.sequence_number;

    if (md.bid_levels.empty() || md.ask_levels.empty()) {
        std::cout << "WARNING: Empty order book detected, skipping quote update\n";
        return;
    }

    // Process fill events for our resting orders
    for (const auto& fill : md.mm_fills) {
        if (active_orders.count(fill.order_id)) {
            on_fill(fill);
        }
    }

    if (!check_risk_limits(md)) {
        return;
    }

    update_quotes(md, simulator);

    market_data_log.push_back(md);
}

void MarketMaker::on_fill(const FillEvent& fill) {
    ++total_fills;

    if (fill.side == Side::BUY) {
        // Our resting BUY was filled — we bought
        inventory += fill.fill_qty;
        cash -= fill.price * fill.fill_qty;
    } else {
        // Our resting SELL was filled — we sold
        inventory -= fill.fill_qty;
        cash += fill.price * fill.fill_qty;
    }

    daily_pnl = cash + inventory * fill.price - 100000.0;

    // Update or remove active order
    auto it = active_orders.find(fill.order_id);
    if (it != active_orders.end()) {
        if (fill.leaves_qty == 0) {
            active_orders.erase(it);
        } else {
            it->second.leaves_qty = fill.leaves_qty;
            it->second.status = OrderStatus::PARTIALLY_FILLED;
        }
    }

    std::cout << "FILL: " << (fill.side == Side::BUY ? "BUY" : "SELL")
              << " " << fill.fill_qty << " @ " << std::fixed << std::setprecision(4)
              << fill.price << " (leaves=" << fill.leaves_qty
              << ") inv=" << inventory << " cash=" << std::setprecision(2) << cash << "\n";
}

bool MarketMaker::check_risk_limits(const MarketDataEvent& md) {
    if (std::abs(inventory) > risk_limits.max_position) {
        return false;
    }
    if (daily_pnl < -risk_limits.max_daily_loss) {
        return false;
    }
    if (md.best_ask_price - md.best_bid_price > risk_limits.max_quote_spread) {
        return false;
    }
    return true;
}

void MarketMaker::cancel_all_orders(MarketSimulator& simulator) {
    for (auto it = active_orders.begin(); it != active_orders.end(); ) {
        simulator.cancel_order(it->first);
        it = active_orders.erase(it);
    }
}

void MarketMaker::update_quotes(const MarketDataEvent& md, MarketSimulator& simulator) {
    const double base_spread = 0.02;

    // Cancel stale orders before placing new ones
    cancel_all_orders(simulator);

    double best_bid = md.bid_levels[0].price;
    double best_ask = md.ask_levels[0].price;
    double mid_price = (best_bid + best_ask) / 2.0;

    double inv_skew = calculate_inventory_skew();

    double bid_price = mid_price - base_spread / 2.0 + inv_skew;
    double ask_price = mid_price + base_spread / 2.0 + inv_skew;

    int bid_size = calculate_optimal_quote_size(md, Side::BUY);
    int ask_size = calculate_optimal_quote_size(md, Side::SELL);

    bid_size = std::max(risk_limits.min_quote_size, std::min(bid_size, risk_limits.max_quote_size));
    ask_size = std::max(risk_limits.min_quote_size, std::min(ask_size, risk_limits.max_quote_size));

    // Submit bid
    std::string bid_id = generate_order_id();
    Order bid_order(bid_id, Side::BUY, bid_price, bid_size, md.timestamp);
    if (simulator.submit_order(bid_order) == OrderStatus::ACKNOWLEDGED) {
        bid_order.status = OrderStatus::ACKNOWLEDGED;
        active_orders.emplace(bid_id, bid_order);
    }

    // Submit ask
    std::string ask_id = generate_order_id();
    Order ask_order(ask_id, Side::SELL, ask_price, ask_size, md.timestamp);
    if (simulator.submit_order(ask_order) == OrderStatus::ACKNOWLEDGED) {
        ask_order.status = OrderStatus::ACKNOWLEDGED;
        active_orders.emplace(ask_id, ask_order);
    }

    last_quote_time = md.timestamp;
}

double MarketMaker::calculate_inventory_skew() const {
    const double skew_factor = 0.001;
    const double max_skew = 0.01;
    double skew = -inventory * skew_factor;
    return std::max(-max_skew, std::min(max_skew, skew));
}

int MarketMaker::calculate_optimal_quote_size(const MarketDataEvent& md, Side side) {
    const int base_size = 5;
    const double size_factor = 0.1;

    int market_depth = 0;
    if (side == Side::BUY && !md.bid_levels.empty()) {
        market_depth = md.bid_levels[0].size;
    } else if (side == Side::SELL && !md.ask_levels.empty()) {
        market_depth = md.ask_levels[0].size;
    }

    double inventory_factor = 1.0 - std::abs(inventory) / static_cast<double>(risk_limits.max_position);
    inventory_factor = std::max(0.1, inventory_factor);

    int scaled_size = static_cast<int>(base_size * (1.0 + market_depth * size_factor) * inventory_factor);
    return std::max(1, std::min(scaled_size, risk_limits.max_quote_size));
}

std::string MarketMaker::generate_order_id() {
    return "MM_" + std::to_string(++order_counter);
}

void MarketMaker::report() {
    if (market_data_log.empty()) {
        std::cout << "No market data events logged. Report cannot be generated." << std::endl;
        return;
    }

    double mark = (market_data_log.back().best_bid_price + market_data_log.back().best_ask_price) / 2.0;
    double unrealized_pnl = inventory * mark;
    double total_pnl = cash + unrealized_pnl - 100000.0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== MARKET MAKER REPORT ===" << std::endl;
    std::cout << "Inventory: " << inventory << " shares" << std::endl;
    std::cout << "Cash: $" << cash << std::endl;
    std::cout << "Mark Price: $" << mark << std::endl;
    std::cout << "Unrealized PnL: $" << unrealized_pnl << std::endl;
    std::cout << "Total PnL: $" << total_pnl << std::endl;
    std::cout << "Total Fills: " << total_fills << std::endl;
    std::cout << "Active Orders: " << active_orders.size() << std::endl;
    std::cout << "Inventory Skew: " << calculate_inventory_skew() << std::endl;
    std::cout << "============================" << std::endl;
}

double MarketMaker::get_cash() const {
    return cash;
}

int MarketMaker::get_inventory() const {
    return inventory;
}

double MarketMaker::get_mark_price() const {
    if (market_data_log.empty()) return 0.0;
    return (market_data_log.back().best_bid_price + market_data_log.back().best_ask_price) / 2.0;
}

double MarketMaker::get_unrealized_pnl() const {
    return get_inventory() * get_mark_price();
}

double MarketMaker::get_total_pnl() const {
    return get_cash() + get_unrealized_pnl() - 100000.0;
}

int MarketMaker::get_total_fills() const {
    return total_fills;
}

double MarketMaker::get_inventory_skew() const {
    const double skew_factor = 0.001;
    const double max_skew = 0.01;
    double skew = -inventory * skew_factor;
    return std::max(-max_skew, std::min(max_skew, skew));
}
