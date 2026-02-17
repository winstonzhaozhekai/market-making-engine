#include "include/WsSession.h"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include "MarketDataEvent.h"
#include "MarketMaker.h"
#include "MarketSimulator.h"
#include "PerformanceModule.h"
#include "include/SimulationConfig.h"

namespace {

std::string trim_copy(const std::string& input) {
    const std::string whitespace = " \t\r\n";
    const auto begin = input.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(whitespace);
    return input.substr(begin, end - begin + 1);
}

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

const char* side_to_string(Side side) {
    return side == Side::BUY ? "BUY" : "SELL";
}

} // namespace

namespace wsproto {

ClientCommand parse_command(const std::string& message) {
    const std::string command = trim_copy(message);
    if (command == "run_simulation") {
        return ClientCommand::RunSimulation;
    }
    if (command == "stop_simulation") {
        return ClientCommand::StopSimulation;
    }
    if (command == "enable_overlap" || command == "set_allow_overlap:true") {
        return ClientCommand::EnableOverlap;
    }
    if (command == "disable_overlap" || command == "set_allow_overlap:false") {
        return ClientCommand::DisableOverlap;
    }
    return ClientCommand::Unknown;
}

CommandAction apply_command(SessionProtocolState& state, ClientCommand command) {
    switch (command) {
        case ClientCommand::RunSimulation:
            if (state.simulation_active && !state.allow_overlap) {
                return CommandAction::RejectOverlap;
            }
            state.simulation_active = true;
            return CommandAction::StartSimulation;
        case ClientCommand::StopSimulation:
            if (!state.simulation_active) {
                return CommandAction::Noop;
            }
            state.simulation_active = false;
            return CommandAction::StopSimulation;
        case ClientCommand::EnableOverlap:
            state.allow_overlap = true;
            return CommandAction::Noop;
        case ClientCommand::DisableOverlap:
            state.allow_overlap = false;
            return CommandAction::Noop;
        case ClientCommand::Unknown:
            return CommandAction::Noop;
    }
    return CommandAction::Noop;
}

bool enqueue_outbound(OutboundQueueState& state, std::string message) {
    state.queue.push_back(std::move(message));
    if (state.write_in_progress) {
        return false;
    }
    state.write_in_progress = true;
    return true;
}

bool complete_outbound_write(OutboundQueueState& state) {
    if (!state.queue.empty()) {
        state.queue.pop_front();
    }
    if (state.queue.empty()) {
        state.write_in_progress = false;
        return false;
    }
    return true;
}

} // namespace wsproto

WsSession::WsSession(tcp::socket&& socket, WsSessionConfig config, CloseCallback on_close)
    : ws_(std::move(socket)),
      executor_(ws_.get_executor()),
      config_(config),
      on_close_(std::move(on_close)),
      allow_overlapping_(config.allow_overlapping_simulations),
      last_activity_(std::chrono::steady_clock::now()),
      heartbeat_timer_(executor_),
      inactivity_timer_(executor_) {}

WsSession::~WsSession() {
    request_stop_all_simulations();
}

void WsSession::start() {
    net::dispatch(executor_, [self = shared_from_this()] {
        self->ws_.set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
        self->ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(boost::beast::http::field::server, "market-making-engine");
            }));

        self->ws_.async_accept([self](boost::beast::error_code ec) {
            self->on_accept(ec);
        });
    });
}

void WsSession::stop() {
    net::post(executor_, [self = shared_from_this()] {
        self->stop_with_reason("session_stop");
    });
}

void WsSession::on_accept(boost::beast::error_code ec) {
    if (ec) {
        stop_with_reason("accept_error:" + ec.message());
        return;
    }

    ws_.control_callback([weak_self = weak_from_this()](websocket::frame_type kind, boost::beast::string_view) {
        if (kind == websocket::frame_type::ping ||
            kind == websocket::frame_type::pong ||
            kind == websocket::frame_type::close) {
            if (auto self = weak_self.lock()) {
                self->last_activity_ = std::chrono::steady_clock::now();
            }
        }
    });

    last_activity_ = std::chrono::steady_clock::now();
    enqueue_outbound_message(make_status_json("connected", "session_ready"));
    start_heartbeat();
    start_inactivity_check();
    do_read();
}

void WsSession::do_read() {
    ws_.async_read(read_buffer_, [self = shared_from_this()](boost::beast::error_code ec, std::size_t bytes_transferred) {
        self->on_read(ec, bytes_transferred);
    });
}

void WsSession::on_read(boost::beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        stop_with_reason("read_error:" + ec.message());
        return;
    }

    const std::string message = boost::beast::buffers_to_string(read_buffer_.data());
    read_buffer_.consume(bytes_transferred);
    last_activity_ = std::chrono::steady_clock::now();

    handle_command(message);
    if (!stopping_) {
        do_read();
    }
}

void WsSession::handle_command(const std::string& message) {
    cleanup_finished_simulations();

    const wsproto::ClientCommand command = wsproto::parse_command(message);
    if (command == wsproto::ClientCommand::Unknown) {
        enqueue_outbound_message(make_error_json("unknown_command"));
        return;
    }

    wsproto::SessionProtocolState state;
    state.simulation_active = has_active_simulation();
    state.allow_overlap = allow_overlapping_;

    const wsproto::CommandAction action = wsproto::apply_command(state, command);
    allow_overlapping_ = state.allow_overlap;

    if (command == wsproto::ClientCommand::EnableOverlap) {
        enqueue_outbound_message(make_status_json("ok", "overlap_enabled"));
        return;
    }
    if (command == wsproto::ClientCommand::DisableOverlap) {
        enqueue_outbound_message(make_status_json("ok", "overlap_disabled"));
        return;
    }

    if (action == wsproto::CommandAction::RejectOverlap) {
        enqueue_outbound_message(make_error_json("simulation_already_running"));
        return;
    }

    if (action == wsproto::CommandAction::StopSimulation) {
        request_stop_all_simulations();
        enqueue_outbound_message(make_status_json("stopped", "simulation_stopped"));
        return;
    }

    if (action == wsproto::CommandAction::StartSimulation) {
        const int run_id = start_simulation_task();
        enqueue_outbound_message(make_status_json("started", "simulation_started", run_id));
    }
}

void WsSession::enqueue_outbound_message(std::string message) {
    net::post(
        executor_,
        [self = shared_from_this(), message = std::move(message)]() mutable {
            if (self->stopping_) {
                return;
            }
            if (wsproto::enqueue_outbound(self->outbound_, std::move(message))) {
                self->do_write();
            }
        });
}

void WsSession::do_write() {
    if (outbound_.queue.empty()) {
        outbound_.write_in_progress = false;
        return;
    }

    ws_.text(true);
    const std::string& message = outbound_.queue.front();
    ws_.async_write(
        net::buffer(message),
        [self = shared_from_this()](boost::beast::error_code ec, std::size_t) {
            self->on_write(ec);
        });
}

void WsSession::on_write(boost::beast::error_code ec) {
    if (ec) {
        stop_with_reason("write_error:" + ec.message());
        return;
    }

    if (wsproto::complete_outbound_write(outbound_)) {
        do_write();
    }
}

void WsSession::start_heartbeat() {
    heartbeat_timer_.expires_after(config_.heartbeat_interval);
    heartbeat_timer_.async_wait([self = shared_from_this()](boost::beast::error_code ec) {
        self->on_heartbeat(ec);
    });
}

void WsSession::on_heartbeat(boost::beast::error_code ec) {
    if (ec || stopping_) {
        return;
    }

    ws_.async_ping({}, [self = shared_from_this()](boost::beast::error_code ping_ec) {
        if (ping_ec) {
            if (ping_ec != net::error::operation_aborted &&
                ping_ec != websocket::error::closed) {
                self->stop_with_reason("ping_error:" + ping_ec.message());
            }
            return;
        }
        self->start_heartbeat();
    });
}

void WsSession::start_inactivity_check() {
    inactivity_timer_.expires_after(config_.heartbeat_interval);
    inactivity_timer_.async_wait([self = shared_from_this()](boost::beast::error_code ec) {
        self->on_inactivity_check(ec);
    });
}

void WsSession::on_inactivity_check(boost::beast::error_code ec) {
    if (ec || stopping_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_activity_ > config_.inactivity_timeout) {
        stop_with_reason("inactivity_timeout");
        return;
    }

    start_inactivity_check();
}

int WsSession::start_simulation_task() {
    ++run_counter_;
    const int run_id = run_counter_;

    auto task = std::make_shared<SimulationTask>();
    {
        std::lock_guard<std::mutex> lock(simulation_mutex_);
        simulation_tasks_.push_back(task);
    }

    task->worker = std::thread([weak_self = weak_from_this(), task, run_id] {
        if (auto self = weak_self.lock()) {
            self->run_simulation(task, run_id);
        } else {
            task->done.store(true, std::memory_order_release);
        }
    });

    return run_id;
}

void WsSession::run_simulation(const std::shared_ptr<SimulationTask>& task, int run_id) {
    try {
        SimulationConfig sim_cfg;
        sim_cfg.latency_ms = config_.simulation_latency_ms;
        sim_cfg.iterations = config_.simulation_iterations;
        sim_cfg.seed = static_cast<uint32_t>(42 + run_id);
        sim_cfg.quiet = true;

        RiskConfig risk_cfg;
        MarketSimulator simulator(sim_cfg);
        MarketMaker mm(risk_cfg);
        PerformanceModule perf(static_cast<std::size_t>(std::max(1, config_.simulation_iterations)));

        const auto wall_start = std::chrono::steady_clock::now();
        int processed = 0;

        for (int iteration = 0; iteration < config_.simulation_iterations; ++iteration) {
            if (stop_requested_.load(std::memory_order_acquire) ||
                task->stop_requested.load(std::memory_order_acquire)) {
                break;
            }

            auto iter_start = std::chrono::steady_clock::now();
            MarketDataEvent md;
            try {
                md = simulator.generate_event();
            } catch (const std::out_of_range&) {
                break;
            }

            mm.on_market_data(md, simulator);
            auto iter_end = std::chrono::steady_clock::now();
            perf.record_latency(
                std::chrono::duration_cast<std::chrono::nanoseconds>(iter_end - iter_start).count());

            ++processed;
            enqueue_outbound_message(
                make_update_json(md, iteration, run_id, false, mm, 0.0, 0.0, processed, 0.0));
        }

        const auto wall_end = std::chrono::steady_clock::now();
        perf.set_wall_time(wall_end - wall_start);
        const double total_runtime_ms =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(wall_end - wall_start).count();
        const double avg_iteration_ms = processed == 0 ? 0.0 : total_runtime_ms / static_cast<double>(processed);

        enqueue_outbound_message(make_update_json(
            MarketDataEvent{},
            processed == 0 ? 0 : processed - 1,
            run_id,
            true,
            mm,
            total_runtime_ms,
            avg_iteration_ms,
            processed,
            perf.throughput()));
    } catch (const std::exception& ex) {
        enqueue_outbound_message(make_error_json(std::string("simulation_error:") + ex.what()));
    }

    task->done.store(true, std::memory_order_release);

    net::post(
        executor_,
        [self = shared_from_this()] {
            self->cleanup_finished_simulations();
        });
}

void WsSession::request_stop_all_simulations() {
    std::vector<std::shared_ptr<SimulationTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex_);
        tasks = simulation_tasks_;
    }

    for (const auto& task : tasks) {
        task->stop_requested.store(true, std::memory_order_release);
    }
    for (const auto& task : tasks) {
        if (task->worker.joinable()) {
            task->worker.join();
        }
        task->done.store(true, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(simulation_mutex_);
        simulation_tasks_.clear();
    }
}

bool WsSession::has_active_simulation() const {
    std::lock_guard<std::mutex> lock(simulation_mutex_);
    return std::any_of(simulation_tasks_.begin(), simulation_tasks_.end(), [](const auto& task) {
        return !task->done.load(std::memory_order_acquire);
    });
}

void WsSession::cleanup_finished_simulations() {
    std::vector<std::shared_ptr<SimulationTask>> finished;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex_);
        auto it = simulation_tasks_.begin();
        while (it != simulation_tasks_.end()) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                finished.push_back(*it);
                it = simulation_tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& task : finished) {
        if (task->worker.joinable()) {
            task->worker.join();
        }
    }
}

void WsSession::stop_with_reason(const std::string& reason) {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    stop_requested_.store(true, std::memory_order_release);
    heartbeat_timer_.cancel();
    inactivity_timer_.cancel();

    (void)reason;
    request_stop_all_simulations();

    if (!ws_.is_open()) {
        notify_closed();
        return;
    }

    ws_.async_close(
        websocket::close_code::normal,
        [self = shared_from_this()](boost::beast::error_code) {
            self->notify_closed();
        });
}

void WsSession::notify_closed() {
    if (close_notified_) {
        return;
    }
    close_notified_ = true;
    if (on_close_) {
        on_close_(shared_from_this());
    }
}

std::string WsSession::make_status_json(const std::string& status, const std::string& message, int run_id) const {
    std::ostringstream out;
    out << "{\"schema_version\":" << config_.schema_version
        << ",\"type\":\"status\""
        << ",\"status\":\"" << json_escape(status) << "\""
        << ",\"message\":\"" << json_escape(message) << "\"";
    if (run_id >= 0) {
        out << ",\"run_id\":" << run_id;
    }
    out << "}";
    return out.str();
}

std::string WsSession::make_error_json(const std::string& message) const {
    std::ostringstream out;
    out << "{\"schema_version\":" << config_.schema_version
        << ",\"type\":\"error\""
        << ",\"message\":\"" << json_escape(message) << "\"}";
    return out.str();
}

std::string WsSession::make_update_json(
    const MarketDataEvent& md,
    int iteration,
    int run_id,
    bool include_metrics,
    const MarketMaker& mm,
    double total_runtime_ms,
    double average_iteration_ms,
    int processed_iterations,
    double throughput_eps) const {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "{\"schema_version\":" << config_.schema_version
        << ",\"type\":\"simulation_update\""
        << ",\"run_id\":" << run_id
        << ",\"iteration\":" << iteration
        << ",\"trades\":[";

    for (std::size_t i = 0; i < md.trades.size(); ++i) {
        const auto& trade = md.trades[i];
        out << "{\"price\":" << trade.price
            << ",\"size\":" << trade.size
            << ",\"side\":\"" << side_to_string(trade.aggressor_side) << "\"}";
        if (i + 1 < md.trades.size()) {
            out << ",";
        }
    }
    out << "]";

    if (include_metrics) {
        out << ",\"metrics\":{"
            << "\"total_iterations\":" << processed_iterations
            << ",\"total_runtime\":" << total_runtime_ms
            << ",\"average_iteration_time\":" << average_iteration_ms
            << ",\"throughput_eps\":" << throughput_eps
            << ",\"inventory\":" << mm.get_inventory()
            << ",\"cash\":" << mm.get_cash()
            << ",\"mark_price\":" << mm.get_mark_price()
            << ",\"realized_pnl\":" << mm.get_realized_pnl()
            << ",\"unrealized_pnl\":" << mm.get_unrealized_pnl()
            << ",\"total_pnl\":" << mm.get_total_pnl()
            << ",\"fees\":" << mm.get_fees()
            << ",\"rebates\":" << mm.get_rebates()
            << ",\"avg_entry_price\":" << mm.get_avg_entry_price()
            << ",\"gross_exposure\":" << mm.get_gross_exposure()
            << ",\"net_exposure\":" << mm.get_net_exposure()
            << ",\"inventory_skew\":" << mm.get_inventory_skew()
            << "}";
    }

    out << "}";
    return out.str();
}
