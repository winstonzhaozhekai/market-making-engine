#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_set>

#include "include/WsSession.h"

namespace net = boost::asio;
using tcp = net::ip::tcp;

class WebSocketServer {
public:
    WebSocketServer(net::io_context& ioc, const tcp::endpoint& endpoint, WsSessionConfig config)
        : acceptor_(ioc), session_config_(config) {
        boost::system::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("acceptor open failed: " + ec.message());
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("set reuse_address failed: " + ec.message());
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("acceptor bind failed: " + ec.message());
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("acceptor listen failed: " + ec.message());
        }
    }

    void run() {
        do_accept();
    }

private:
    tcp::acceptor acceptor_;
    WsSessionConfig session_config_;
    std::unordered_set<std::shared_ptr<WsSession>> sessions_;

    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<WsSession>(
                    std::move(socket),
                    session_config_,
                    [this](const std::shared_ptr<WsSession>& closed_session) {
                        sessions_.erase(closed_session);
                    });
                sessions_.insert(session);
                session->start();
            }
            do_accept();
        });
    }
};

int main() {
    try {
        net::io_context ioc;
        WsSessionConfig session_config;
        session_config.allow_overlapping_simulations = false;
        session_config.heartbeat_interval = std::chrono::seconds(5);
        session_config.inactivity_timeout = std::chrono::seconds(30);

        WebSocketServer server(ioc, tcp::endpoint(tcp::v4(), 8080), session_config);
        std::cout << "WebSocket server starting on port 8080..." << std::endl;
        server.run();
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
