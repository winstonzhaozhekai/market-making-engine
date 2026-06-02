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

#include "Instrument.h"
#include "RiskManager.h"
#include "SimulationConfig.h"

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

// Default outbound queue capacity. A slow client cannot make the simulation
// worker thread accumulate updates without bound: once the queue is full,
// new messages are dropped and `OutboundQueueState::dropped_count` is
// incremented. Operators can detect drops via the server-side stderr log
// emitted by `WsSession::enqueue_outbound_message`.
constexpr std::size_t kDefaultOutboundQueueCapacity = 1024;

struct OutboundQueueState {
    bool write_in_progress = false;
    std::deque<std::string> queue;
    std::size_t max_queue_size = kDefaultOutboundQueueCapacity;
    uint64_t dropped_count = 0;
};

ClientCommand parse_command(const std::string& message);
CommandAction apply_command(SessionProtocolState& state, ClientCommand command);
// Returns true iff the caller should kick off `do_write()` (i.e. the message
// was enqueued AND no write was previously in flight). Returns false if the
// message was dropped due to capacity OR if a write is already in flight.
// On drop, `state.dropped_count` is incremented and `state.queue` is unchanged.
bool enqueue_outbound(OutboundQueueState& state, std::string message);
bool complete_outbound_write(OutboundQueueState& state);

} // namespace wsproto

struct WsSessionConfig {
    bool allow_overlapping_simulations = false;
    int simulation_iterations = 1000;
    int simulation_latency_ms = 10;
    uint32_t simulation_seed = 42;
    std::string strategy_name = "heuristic";
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
    // Threading invariant: `stopping_` is touched only on the executor thread.
    // All call sites verified 2026-05-02:
    //   - `on_read`, `on_heartbeat`, `on_inactivity_check` run as Asio handlers
    //     on the executor.
    //   - `enqueue_outbound_message` reads `stopping_` only inside a lambda
    //     posted via `net::post(executor_, ...)` (so the read runs on the
    //     executor too, not on the calling worker thread).
    //   - `stop_with_reason` writes `stopping_` and is invoked either directly
    //     from an executor handler or via `net::post(executor_, ...)`.
    // Because the `io_context` is run from a single thread (see
    // `WebSocketServer::run`), `stopping_` is effectively single-threaded and
    // does not need to be atomic. Re-validate this invariant under TSan when
    // the toolchain milestone (M2) lands; if the io_context ever runs on a
    // thread pool, `stopping_` must become `std::atomic<bool>`.
    bool stopping_ = false;
    bool allow_overlapping_ = false;
    std::atomic<bool> stop_requested_{false};
    bool outbound_overflow_logged_ = false;
    std::chrono::steady_clock::time_point last_activity_;
    net::steady_timer heartbeat_timer_;
    net::steady_timer inactivity_timer_;
    SimulationConfig next_simulation_config_;
    RiskConfig next_risk_config_;
    std::string next_strategy_name_;

    int run_counter_ = 0;
    std::vector<std::shared_ptr<SimulationTask>> simulation_tasks_;
    mutable std::mutex simulation_mutex_;

    void on_accept(boost::beast::error_code ec);
    void do_read();
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void handle_command(const std::string& message);
    bool handle_set_command(const std::string& message);

    void enqueue_outbound_message(std::string message);
    void do_write();
    void on_write(boost::beast::error_code ec);

    void start_heartbeat();
    void on_heartbeat(boost::beast::error_code ec);
    void start_inactivity_check();
    void on_inactivity_check(boost::beast::error_code ec);

    int start_simulation_task();
    void run_simulation(
        const std::shared_ptr<SimulationTask>& task,
        int run_id,
        SimulationConfig sim_cfg,
        RiskConfig risk_cfg,
        std::string strategy_name);
    void request_stop_all_simulations();
    bool has_active_simulation() const;
    void cleanup_finished_simulations();

    void stop_with_reason(const std::string& reason);
    void notify_closed();

    std::string make_status_json(const std::string& status, const std::string& message, int run_id = -1) const;
    std::string make_error_json(const std::string& message) const;
    std::string make_update_json(
        const MarketDataEvent& md,
        const Instrument& instrument,
        int iteration,
        int run_id,
        bool is_final,
        const MarketMaker& mm,
        double total_runtime_ms,
        double average_iteration_ms,
        int processed_iterations,
        double throughput_eps) const;
};

#endif // WS_SESSION_H
