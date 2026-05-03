#include <gtest/gtest.h>
#include <string>

#include "include/WsSession.h"

namespace {

TEST(WsProtocol, test_command_parsing) {
    EXPECT_EQ(wsproto::parse_command("run_simulation"), wsproto::ClientCommand::RunSimulation);
    EXPECT_EQ(wsproto::parse_command(" stop_simulation "), wsproto::ClientCommand::StopSimulation);
    EXPECT_EQ(wsproto::parse_command("enable_overlap"), wsproto::ClientCommand::EnableOverlap);
    EXPECT_EQ(wsproto::parse_command("set_allow_overlap:false"), wsproto::ClientCommand::DisableOverlap);
    EXPECT_EQ(wsproto::parse_command("unknown"), wsproto::ClientCommand::Unknown);
}

TEST(WsProtocol, test_overlap_guard_behavior) {
    wsproto::SessionProtocolState state;
    state.simulation_active = true;
    state.allow_overlap = false;

    auto action = wsproto::apply_command(state, wsproto::ClientCommand::RunSimulation);
    EXPECT_EQ(action, wsproto::CommandAction::RejectOverlap);
    EXPECT_TRUE(state.simulation_active);

    action = wsproto::apply_command(state, wsproto::ClientCommand::EnableOverlap);
    EXPECT_EQ(action, wsproto::CommandAction::Noop);
    EXPECT_TRUE(state.allow_overlap);

    action = wsproto::apply_command(state, wsproto::ClientCommand::RunSimulation);
    EXPECT_EQ(action, wsproto::CommandAction::StartSimulation);
    EXPECT_TRUE(state.simulation_active);

    action = wsproto::apply_command(state, wsproto::ClientCommand::StopSimulation);
    EXPECT_EQ(action, wsproto::CommandAction::StopSimulation);
    EXPECT_FALSE(state.simulation_active);
}

TEST(WsProtocol, test_outbound_queue_serialization_state_machine) {
    wsproto::OutboundQueueState state;

    EXPECT_TRUE(wsproto::enqueue_outbound(state, "{\"msg\":1}"));
    EXPECT_TRUE(state.write_in_progress);
    EXPECT_EQ(state.queue.size(), 1u);

    EXPECT_FALSE(wsproto::enqueue_outbound(state, "{\"msg\":2}"));
    EXPECT_TRUE(state.write_in_progress);
    EXPECT_EQ(state.queue.size(), 2u);

    EXPECT_TRUE(wsproto::complete_outbound_write(state));
    EXPECT_TRUE(state.write_in_progress);
    EXPECT_EQ(state.queue.size(), 1u);

    EXPECT_FALSE(wsproto::complete_outbound_write(state));
    EXPECT_FALSE(state.write_in_progress);
    EXPECT_TRUE(state.queue.empty());
}

// C8b regression: outbound queue caps at max_queue_size; overflow drops
// the new message, increments dropped_count, never grows past the cap.
TEST(WsProtocol, test_outbound_queue_drops_on_overflow) {
    wsproto::OutboundQueueState state;
    state.max_queue_size = 3;

    EXPECT_TRUE(wsproto::enqueue_outbound(state, "a"));
    EXPECT_FALSE(wsproto::enqueue_outbound(state, "b"));
    EXPECT_FALSE(wsproto::enqueue_outbound(state, "c"));
    EXPECT_EQ(state.queue.size(), 3u);
    EXPECT_EQ(state.dropped_count, 0u);

    EXPECT_FALSE(wsproto::enqueue_outbound(state, "d"));
    EXPECT_EQ(state.queue.size(), 3u);
    EXPECT_EQ(state.dropped_count, 1u);

    EXPECT_FALSE(wsproto::enqueue_outbound(state, "e"));
    EXPECT_EQ(state.queue.size(), 3u);
    EXPECT_EQ(state.dropped_count, 2u);

    wsproto::complete_outbound_write(state);
    EXPECT_EQ(state.queue.size(), 2u);
    EXPECT_TRUE(state.write_in_progress);

    EXPECT_FALSE(wsproto::enqueue_outbound(state, "f"));
    EXPECT_EQ(state.queue.size(), 3u);
    EXPECT_EQ(state.dropped_count, 2u);

    EXPECT_EQ(state.queue.front(), "b");
}

TEST(WsProtocol, test_outbound_queue_default_capacity_is_generous) {
    wsproto::OutboundQueueState state;
    EXPECT_EQ(state.max_queue_size, wsproto::kDefaultOutboundQueueCapacity);
    EXPECT_GE(state.max_queue_size, 1024u);
    EXPECT_EQ(state.dropped_count, 0u);
}

}  // namespace
