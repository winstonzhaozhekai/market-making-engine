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

// C8b regression: outbound queue must cap at max_queue_size; overflow drops
// the new message, increments dropped_count, and never grows past the cap.
void test_outbound_queue_drops_on_overflow() {
    wsproto::OutboundQueueState state;
    state.max_queue_size = 3;  // tiny cap for test

    // Three pushes: first should start a write, next two queue up.
    assert(wsproto::enqueue_outbound(state, "a") == true);
    assert(wsproto::enqueue_outbound(state, "b") == false);
    assert(wsproto::enqueue_outbound(state, "c") == false);
    assert(state.queue.size() == 3);
    assert(state.dropped_count == 0);

    // Fourth push: queue is at cap → drop new, increment counter, queue size
    // unchanged, return false (no new write to start).
    const bool fourth_should_start = wsproto::enqueue_outbound(state, "d");
    assert(fourth_should_start == false);
    assert(state.queue.size() == 3);
    assert(state.dropped_count == 1);

    // Fifth push: another drop.
    assert(wsproto::enqueue_outbound(state, "e") == false);
    assert(state.queue.size() == 3);
    assert(state.dropped_count == 2);

    // Drain one: queue size = 2, capacity available again.
    wsproto::complete_outbound_write(state);
    assert(state.queue.size() == 2);
    assert(state.write_in_progress);

    // Sixth push: room again; enqueue succeeds. write_in_progress already
    // true, so the call returns false but the message IS queued.
    assert(wsproto::enqueue_outbound(state, "f") == false);
    assert(state.queue.size() == 3);
    assert(state.dropped_count == 2);  // unchanged: not a drop

    // Verify FIFO ordering preserved across the drop/drain sequence:
    // queue front should still be the oldest non-popped element ("b").
    assert(state.queue.front() == "b");

    std::cout << "PASS: test_outbound_queue_drops_on_overflow\n";
}

// C8b regression: default capacity is high enough that normal traffic
// never overflows — the existing serialization-state-machine test already
// implicitly relies on this. Just lock it in explicitly.
void test_outbound_queue_default_capacity_is_generous() {
    wsproto::OutboundQueueState state;
    assert(state.max_queue_size == wsproto::kDefaultOutboundQueueCapacity);
    assert(state.max_queue_size >= 1024);
    assert(state.dropped_count == 0);
    std::cout << "PASS: test_outbound_queue_default_capacity_is_generous\n";
}

} // namespace

int main() {
    test_command_parsing();
    test_overlap_guard_behavior();
    test_outbound_queue_serialization_state_machine();
    test_outbound_queue_drops_on_overflow();
    test_outbound_queue_default_capacity_is_generous();
    std::cout << "WebSocket protocol tests passed.\n";
    return 0;
}
