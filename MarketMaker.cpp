#include "MarketMaker.h"
#include <iostream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <cmath>

MarketMaker::MarketMaker() {
    last_quote_time = std::chrono::system_clock::now();
}

void MarketMaker::on_market_data(const MarketDataEvent& md) {
    // Check for sequence gaps (missed data)
    if (md.sequence_number != last_processed_sequence + 1 && last_processed_sequence != 0) {
        missed_opportunities++;
        std::cout << "WARNING: Sequence gap detected. Missed " 
                  << (md.sequence_number - last_processed_sequence - 1) 
                  << " events\n";
    }
    last_processed_sequence = md.sequence_number;
    
    // Check for empty order book
    if (md.bid_levels.empty() || md.ask_levels.empty()) {
        std::cout << "WARNING: Empty order book detected, skipping quote update\n";
        return;
    }
    
    // Process any trades and partial fills first
    process_trades(md.trades);
    process_partial_fills(md.partial_fills);
    
    // Check risk limits before quoting
    if (!check_risk_limits(md)) {
        std::cout << "Risk limits exceeded, not quoting\n";
        return;
    }
    
    // Update quotes
    update_quotes(md);
    
    // Add event to market_data_log
    std::cout << "Adding MarketDataEvent to log: Sequence " << md.sequence_number << std::endl;
    market_data_log.push_back(md);
}

bool MarketMaker::check_risk_limits(const MarketDataEvent& md) {
    std::cout << "Checking risk limits..." << std::endl;
    std::cout << "Current Inventory: " << inventory << std::endl;
    std::cout << "Current Daily PnL: " << daily_pnl << std::endl;
    std::cout << "Current Spread: " << md.best_ask_price - md.best_bid_price << std::endl;

    // Position limits
    if (std::abs(inventory) > risk_limits.max_position) {
        std::cout << "Risk limit exceeded: Inventory (" << inventory 
                  << ") exceeds max position (" << risk_limits.max_position << ")\n";
        return false;
    }
    
    // Daily loss limits
    if (daily_pnl < -risk_limits.max_daily_loss) {
        std::cout << "Risk limit exceeded: Daily PnL (" << daily_pnl 
                  << ") exceeds max daily loss (" << risk_limits.max_daily_loss << ")\n";
        return false;
    }
    
    // Spread limits
    if (md.best_ask_price - md.best_bid_price > risk_limits.max_quote_spread) {
        std::cout << "Risk limit exceeded: Spread (" << md.best_ask_price - md.best_bid_price 
                  << ") exceeds max quote spread (" << risk_limits.max_quote_spread << ")\n";
        return false;
    }
    
    std::cout << "Risk limits passed." << std::endl;
    return true;
}

void MarketMaker::update_quotes(const MarketDataEvent& md) {
    const double base_spread = 0.02;
    const double latency_threshold_ms = 50.0; // Max latency before we skip quoting
    
    // Check if we're too slow to quote effectively
    auto now = std::chrono::system_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - md.timestamp).count();
    
    if (latency > latency_threshold_ms) {
        missed_opportunities++;
        std::cout << "High latency detected (" << latency << "ms), skipping quote\n";
        return;
    }
    
    double best_bid = md.bid_levels[0].price;
    double best_ask = md.ask_levels[0].price;
    double mid_price = (best_bid + best_ask) / 2.0;
    
    // Apply inventory skewing
    double inventory_skew = calculate_inventory_skew();
    
    // Calculate quote prices with skewing
    double bid_price = mid_price - base_spread / 2.0 + inventory_skew;
    double ask_price = mid_price + base_spread / 2.0 + inventory_skew;
    
    // Calculate optimal quote sizes
    int bid_size = calculate_optimal_quote_size(md, "BID");
    int ask_size = calculate_optimal_quote_size(md, "ASK");
    
    // Ensure sizes are within limits
    bid_size = std::max(risk_limits.min_quote_size, 
                       std::min(bid_size, risk_limits.max_quote_size));
    ask_size = std::max(risk_limits.min_quote_size, 
                       std::min(ask_size, risk_limits.max_quote_size));
    
    // Update order book
    order_book["BID"] = {bid_price, bid_size, generate_order_id(), now};
    order_book["ASK"] = {ask_price, ask_size, generate_order_id(), now};
    
    last_quote_time = now;
    
    std::cout << "Updated quotes: BID " << std::fixed << std::setprecision(4) 
              << bid_price << " x " << bid_size 
              << " | ASK " << ask_price << " x " << ask_size 
              << " | Skew: " << inventory_skew << std::endl;
}

double MarketMaker::calculate_inventory_skew() {
    const double skew_factor = 0.001; // Adjust based on risk appetite
    const double max_skew = 0.01;
    
    double skew = -inventory * skew_factor;
    return std::max(-max_skew, std::min(max_skew, skew));
}

int MarketMaker::calculate_optimal_quote_size(const MarketDataEvent& md, const std::string& side) {
    const int base_size = 5;
    const double size_factor = 0.1;
    
    // Scale size based on market depth
    int market_depth = 0;
    if (side == "BID" && !md.bid_levels.empty()) {
        market_depth = md.bid_levels[0].size;
    } else if (side == "ASK" && !md.ask_levels.empty()) {
        market_depth = md.ask_levels[0].size;
    }
    
    // Scale size based on inventory (quote less aggressively when heavily positioned)
    double inventory_factor = 1.0 - std::abs(inventory) / static_cast<double>(risk_limits.max_position);
    inventory_factor = std::max(0.1, inventory_factor);
    
    int scaled_size = static_cast<int>(base_size * (1.0 + market_depth * size_factor) * inventory_factor);
    
    return std::max(1, std::min(scaled_size, risk_limits.max_quote_size));
}

void MarketMaker::process_trades(const std::vector<Trade>& trades) {
    for (const auto& trade : trades) {
        // Simulate partial fills (30% chance of partial fill)
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_real_distribution<> dis(0.0, 1.0);
        
        bool is_partial = dis(gen) < 0.3;
        int fill_size = is_partial ? std::max(1, trade.size / 2) : trade.size;
        
        // Apply slippage
        double adjusted_price = calculate_slippage(trade.price, fill_size, trade.aggressor_side);
        
        double trade_pnl = 0.0; // Profit or loss from this trade
        
        if (trade.aggressor_side == "BUY") {
            inventory -= fill_size;
            cash += adjusted_price * fill_size;
            trade_pnl = -adjusted_price * fill_size; // Buying reduces PnL
        } else if (trade.aggressor_side == "SELL") {
            inventory += fill_size;
            cash -= adjusted_price * fill_size;
            trade_pnl = adjusted_price * fill_size; // Selling increases PnL
        }
        
        // Update daily PnL
        daily_pnl += trade_pnl;
        
        std::cout << "Processed " << (is_partial ? "partial " : "") << "trade: " 
                  << trade.aggressor_side << " " << fill_size << " @ " 
                  << std::fixed << std::setprecision(4) << adjusted_price 
                  << " (slippage: " << std::abs(adjusted_price - trade.price) << ")\n";
    }
}

void MarketMaker::process_partial_fills(const std::vector<PartialFillEvent>& fills) {
    for (const auto& fill : fills) {
        // Update our records for partial fills
        std::cout << "Partial fill: " << fill.order_id 
                  << " filled " << fill.filled_size 
                  << " remaining " << fill.remaining_size << std::endl;
        
        // Update cash and inventory based on fill
        // Implementation depends on whether it's a buy or sell order
        // This would need order tracking to determine side
    }
}

double MarketMaker::calculate_slippage(double price, int size, const std::string& side) {
    const double slippage_rate = 0.0001; // 1 basis point per unit
    const double max_slippage = 0.005; // 50 basis points max
    
    double slippage = std::min(slippage_rate * size, max_slippage);
    total_slippage += slippage;
    
    if (side == "BUY") {
        return price * (1.0 + slippage); // Pay more when buying
    } else {
        return price * (1.0 - slippage); // Receive less when selling
    }
}

std::string MarketMaker::generate_order_id() {
    return "ORDER_" + std::to_string(++order_counter);
}

void MarketMaker::report() {
    if (market_data_log.empty()) {
        std::cout << "No market data events logged. Report cannot be generated." << std::endl;
        return;
    }
    
    double mark = (market_data_log.back().best_bid_price + market_data_log.back().best_ask_price) / 2.0;
    double unrealized_pnl = inventory * mark;
    double total_pnl = cash + unrealized_pnl - 100000.0; // Subtract initial cash
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== MARKET MAKER REPORT ===" << std::endl;
    std::cout << "Inventory: " << inventory << " shares" << std::endl;
    std::cout << "Cash: $" << cash << std::endl;
    std::cout << "Mark Price: $" << mark << std::endl;
    std::cout << "Unrealized PnL: $" << unrealized_pnl << std::endl;
    std::cout << "Total PnL: $" << total_pnl << std::endl;
    std::cout << "Total Slippage: $" << total_slippage << std::endl;
    std::cout << "Missed Opportunities: " << missed_opportunities << std::endl;
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
    return get_cash() + get_unrealized_pnl() - 100000.0; // Subtract initial cash
}

double MarketMaker::get_total_slippage() const {
    return total_slippage;
}

int MarketMaker::get_missed_opportunities() const {
    return missed_opportunities;
}

double MarketMaker::get_inventory_skew() const {
    const double skew_factor = 0.001; // Adjust based on risk appetite
    const double max_skew = 0.01;
    double skew = -inventory * skew_factor;
    return std::max(-max_skew, std::min(max_skew, skew));
}