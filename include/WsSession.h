#ifndef WS_SESSION_H
#define WS_SESSION_H

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace websocket = boost::beast::websocket;

struct MarketDataEvent;
class MarketMaker;

namespace wsproto {

constexpr int kSchemaVersion = 1;

enum class ClientCommand {
    RunSimulation,
    StopSimulation,
    EnableOverlap,
    DisableOverlap,
    Unknown
};

enum class CommandAction {
    StartSimulation,
    StopSimulation,
    RejectOverlap,
    Noop
};

struct SessionProtocolState {
    bool simulation_active = false;
    bool allow_overlap = false;
};

struct OutboundQueueState {
    bool write_in_progress = false;
    std::deque<std::string> queue;
};

ClientCommand parse_command(const std::string& message);
CommandAction apply_command(SessionProtocolState& state, ClientCommand command);
bool enqueue_outbound(OutboundQueueState& state, std::string message);
bool complete_outbound_write(OutboundQueueState& state);

} // namespace wsproto

struct WsSessionConfig {
    bool allow_overlapping_simulations = false;
    int simulation_iterations = 1000;
    int simulation_latency_ms = 10;
    std::chrono::seconds heartbeat_interval{5};
    std::chrono::seconds inactivity_timeout{30};
    int schema_version = wsproto::kSchemaVersion;
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    using CloseCallback = std::function<void(const std::shared_ptr<WsSession>&)>;

    WsSession(tcp::socket&& socket, WsSessionConfig config, CloseCallback on_close);
    ~WsSession();

    void start();
    void stop();

private:
    struct SimulationTask {
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> done{false};
        std::thread worker;
    };

    websocket::stream<tcp::socket> ws_;
    net::any_io_executor executor_;
    boost::beast::flat_buffer read_buffer_;
    wsproto::OutboundQueueState outbound_;
    WsSessionConfig config_;
    CloseCallback on_close_;

    bool close_notified_ = false;
    bool stopping_ = false;
    bool allow_overlapping_ = false;
    std::atomic<bool> stop_requested_{false};
    std::chrono::steady_clock::time_point last_activity_;
    net::steady_timer heartbeat_timer_;
    net::steady_timer inactivity_timer_;

    int run_counter_ = 0;
    std::vector<std::shared_ptr<SimulationTask>> simulation_tasks_;
    mutable std::mutex simulation_mutex_;

    void on_accept(boost::beast::error_code ec);
    void do_read();
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void handle_command(const std::string& message);

    void enqueue_outbound_message(std::string message);
    void do_write();
    void on_write(boost::beast::error_code ec);

    void start_heartbeat();
    void on_heartbeat(boost::beast::error_code ec);
    void start_inactivity_check();
    void on_inactivity_check(boost::beast::error_code ec);

    int start_simulation_task();
    void run_simulation(const std::shared_ptr<SimulationTask>& task, int run_id);
    void request_stop_all_simulations();
    bool has_active_simulation() const;
    void cleanup_finished_simulations();

    void stop_with_reason(const std::string& reason);
    void notify_closed();

    std::string make_status_json(const std::string& status, const std::string& message, int run_id = -1) const;
    std::string make_error_json(const std::string& message) const;
    std::string make_update_json(
        const MarketDataEvent& md,
        int iteration,
        int run_id,
        bool include_metrics,
        const MarketMaker& mm,
        double total_runtime_ms,
        double average_iteration_ms,
        int processed_iterations,
        double throughput_eps) const;
};

#endif // WS_SESSION_H
