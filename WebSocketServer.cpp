#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <thread>
#include <iostream>
#include <sstream>
#include <chrono>
#include <memory>
#include <vector>

// Include the necessary headers
#include "MarketSimulator.h"
#include "MarketMaker.h"
#include "PerformanceModule.h"
#include "MarketDataEvent.h"

namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace websocket = boost::beast::websocket;
using net::io_context;
using websocket::stream;

class WebSocketServer {
public:
    WebSocketServer(io_context& ioc, tcp::endpoint endpoint)
        : acceptor_(ioc, endpoint) {}

    void run() {
        accept_connection();
    }

private:
    tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> clients_;

    void accept_connection() {
        auto socket = std::make_shared<tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(*socket));
                ws->async_accept([this, ws](boost::system::error_code ec) {
                    if (!ec) {
                        clients_.push_back(ws);
                        listen_to_client(ws);
                    }
                });
            }
            accept_connection();
        });
    }

    void listen_to_client(std::shared_ptr<websocket::stream<tcp::socket>> ws) {
        auto buffer = std::make_shared<boost::beast::flat_buffer>();
        ws->async_read(*buffer, [this, ws, buffer](boost::system::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                std::string message = boost::beast::buffers_to_string(buffer->data());
                handle_message(ws, message);
                buffer->consume(bytes_transferred);
                listen_to_client(ws);
            } else {
                // Remove client from clients_ vector when connection is closed
                auto it = std::find(clients_.begin(), clients_.end(), ws);
                if (it != clients_.end()) {
                    clients_.erase(it);
                }
            }
        });
    }

    void handle_message(std::shared_ptr<websocket::stream<tcp::socket>> ws, const std::string& message) {
        if (message == "run_simulation") {
            std::thread([this, ws]() {
                run_simulation(ws);
            }).detach();
        }
    }

    void run_simulation(std::shared_ptr<websocket::stream<tcp::socket>> ws) {
        MarketSimulator simulator("XYZ", 100.0, 0.1, 0.5, 10);
        MarketMaker mm;
        PerformanceModule perf;

        for (int iteration = 0; iteration < 1000; ++iteration) {
            auto md = simulator.generate_event();
            perf.track_event(md);
            mm.on_market_data(md);

            std::ostringstream data_stream;
            data_stream << "{";
            data_stream << "\"iteration\": " << iteration << ",";
            data_stream << "\"trades\": [";
            for (size_t i = 0; i < md.trades.size(); ++i) {
                const auto& trade = md.trades[i];
                data_stream << "{"
                            << "\"price\": " << trade.price << ","
                            << "\"size\": " << trade.size << ","
                            << "\"side\": \"" << trade.aggressor_side << "\""
                            << "}";
                if (i < md.trades.size() - 1) {
                    data_stream << ",";
                }
            }
            data_stream << "]";

            if (iteration == 999) { // Final metrics
                data_stream << ",\"metrics\": {"
                            << "\"total_iterations\": " << iteration + 1 << ","
                            << "\"total_runtime\": " << perf.get_total_runtime() << ","
                            << "\"average_iteration_time\": " << perf.get_average_iteration_time() << ","
                            << "\"inventory\": " << mm.get_inventory() << ","
                            << "\"cash\": " << mm.get_cash() << ","
                            << "\"mark_price\": " << mm.get_mark_price() << ","
                            << "\"unrealized_pnl\": " << mm.get_unrealized_pnl() << ","
                            << "\"total_pnl\": " << mm.get_total_pnl() << ","
                            << "\"total_slippage\": " << mm.get_total_slippage() << ","
                            << "\"missed_opportunities\": " << mm.get_missed_opportunities() << ","
                            << "\"inventory_skew\": " << mm.get_inventory_skew()
                            << "}";
            }

            data_stream << "}";

            ws->async_write(net::buffer(data_stream.str()), [](boost::system::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "Write error: " << ec.message() << std::endl;
                }
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// Example main function
int main() {
    try {
        io_context ioc;
        tcp::endpoint endpoint(tcp::v4(), 8080);
        WebSocketServer server(ioc, endpoint);
        
        std::cout << "WebSocket server starting on port 8080..." << std::endl;
        server.run();
        ioc.run();
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    return 0;
}