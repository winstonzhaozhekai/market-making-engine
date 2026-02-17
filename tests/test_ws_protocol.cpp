#include <cassert>
#include <iostream>
#include <string>

#include "include/WsSession.h"

namespace {

void test_command_parsing() {
    assert(wsproto::parse_command("run_simulation") == wsproto::ClientCommand::RunSimulation);
    assert(wsproto::parse_command(" stop_simulation ") == wsproto::ClientCommand::StopSimulation);
    assert(wsproto::parse_command("enable_overlap") == wsproto::ClientCommand::EnableOverlap);
    assert(wsproto::parse_command("set_allow_overlap:false") == wsproto::ClientCommand::DisableOverlap);
    assert(wsproto::parse_command("unknown") == wsproto::ClientCommand::Unknown);
    std::cout << "PASS: test_command_parsing\n";
}

void test_overlap_guard_behavior() {
    wsproto::SessionProtocolState state;
    state.simulation_active = true;
    state.allow_overlap = false;

    auto action = wsproto::apply_command(state, wsproto::ClientCommand::RunSimulation);
    assert(action == wsproto::CommandAction::RejectOverlap);
    assert(state.simulation_active);

    action = wsproto::apply_command(state, wsproto::ClientCommand::EnableOverlap);
    assert(action == wsproto::CommandAction::Noop);
    assert(state.allow_overlap);

    action = wsproto::apply_command(state, wsproto::ClientCommand::RunSimulation);
    assert(action == wsproto::CommandAction::StartSimulation);
    assert(state.simulation_active);

    action = wsproto::apply_command(state, wsproto::ClientCommand::StopSimulation);
    assert(action == wsproto::CommandAction::StopSimulation);
    assert(!state.simulation_active);

    std::cout << "PASS: test_overlap_guard_behavior\n";
}

void test_outbound_queue_serialization_state_machine() {
    wsproto::OutboundQueueState state;

    const bool first_should_start = wsproto::enqueue_outbound(state, "{\"msg\":1}");
    assert(first_should_start);
    assert(state.write_in_progress);
    assert(state.queue.size() == 1);

    const bool second_should_start = wsproto::enqueue_outbound(state, "{\"msg\":2}");
    assert(!second_should_start);
    assert(state.write_in_progress);
    assert(state.queue.size() == 2);

    const bool continue_after_first = wsproto::complete_outbound_write(state);
    assert(continue_after_first);
    assert(state.write_in_progress);
    assert(state.queue.size() == 1);

    const bool continue_after_second = wsproto::complete_outbound_write(state);
    assert(!continue_after_second);
    assert(!state.write_in_progress);
    assert(state.queue.empty());

    std::cout << "PASS: test_outbound_queue_serialization_state_machine\n";
}

} // namespace

int main() {
    test_command_parsing();
    test_overlap_guard_behavior();
    test_outbound_queue_serialization_state_machine();
    std::cout << "WebSocket protocol tests passed.\n";
    return 0;
}
