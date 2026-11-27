// Tests for the core <-> protobuf translations in convert.hpp.

#include <chrono>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {
namespace {

TEST(ConvertTest, ChangeFromProtoRejectsSnapshotsAndMissingOps) {
    v1::ReplicateRequest request;
    EXPECT_FALSE(change_from_proto(request).has_value());  // no op set

    request.mutable_snapshot();
    EXPECT_FALSE(change_from_proto(request).has_value());
}

TEST(ConvertTest, LobbySurvivesARoundTripThroughProto) {
    const core::Lobby lobby{"node-a-7", core::make_queue_key("eu", "duos", 1300),
                            {{"bob", 1320}, {"alice", 1290}}, core::Clock::now()};
    v1::Lobby proto;
    to_proto(lobby, &proto);
    const core::Lobby back = from_proto(proto);

    EXPECT_EQ(back.lobby_id, "node-a-7");
    EXPECT_EQ(back.queue, lobby.queue);
    EXPECT_EQ(back.players, lobby.players);  // same players, same order
    // The wall-clock detour can shift the time slightly, but not by much.
    EXPECT_LT(std::chrono::abs(back.formed_at - lobby.formed_at), std::chrono::seconds(1));
}

TEST(ConvertTest, AnEnqueueChangeSurvivesARoundTripThroughProto) {
    const core::ShardChange change{
        core::make_queue_key("us-west", "ranked", 2100), 42,
        core::PlayerJoined{{"carol", 2140}, core::Clock::now(), "req-9"}};
    v1::ReplicateRequest proto;
    to_proto(change, &proto);
    const auto back = change_from_proto(proto);

    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->queue, change.queue);
    EXPECT_EQ(back->sequence, 42u);
    const auto* joined = std::get_if<core::PlayerJoined>(&back->op);
    ASSERT_NE(joined, nullptr);
    EXPECT_EQ(joined->player, (core::Player{"carol", 2140}));
    EXPECT_EQ(joined->request_id, "req-9");
}

TEST(ConvertTest, ACancelChangeSurvivesARoundTripThroughProto) {
    const core::ShardChange change{core::make_queue_key("eu", "duos", 700), 7,
                                   core::PlayerLeft{"dave"}};
    v1::ReplicateRequest proto;
    to_proto(change, &proto);
    const auto back = change_from_proto(proto);

    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->queue, change.queue);
    EXPECT_EQ(back->sequence, 7u);
    const auto* left = std::get_if<core::PlayerLeft>(&back->op);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->player_id, "dave");
}

TEST(ConvertTest, AShardStateSurvivesARoundTripThroughProto) {
    const auto queue = core::make_queue_key("eu", "duos", 1300);
    const auto now = core::Clock::now();
    core::ShardState state{queue, 12, {}, {}, {}, {}};
    state.waiting.push_back(core::PlayerJoined{{"erin", 1310}, now, "req-1"});
    state.waiting.push_back(core::PlayerJoined{{"finn", 1305}, now, ""});
    state.lobbies.push_back(core::Lobby{"node-a-3", queue, {{"gus", 1290}, {"hal", 1330}}, now});
    state.lobby_of = {{"gus", "node-a-3"}, {"hal", "node-a-3"}};
    state.tickets = {{"erin", core::JoinTicket{"req-1", now}}};

    v1::ReplicateRequest proto;
    to_proto(state, &proto);
    const core::ShardState back = state_from_proto(proto);

    EXPECT_EQ(back.queue, queue);
    EXPECT_EQ(back.sequence, 12u);
    ASSERT_EQ(back.waiting.size(), 2u);  // still in arrival order
    EXPECT_EQ(back.waiting[0].player, (core::Player{"erin", 1310}));
    EXPECT_EQ(back.waiting[0].request_id, "req-1");
    EXPECT_EQ(back.waiting[1].player, (core::Player{"finn", 1305}));
    ASSERT_EQ(back.lobbies.size(), 1u);
    EXPECT_EQ(back.lobbies[0].lobby_id, "node-a-3");
    EXPECT_EQ(back.lobby_of, state.lobby_of);
    ASSERT_EQ(back.tickets.count("erin"), 1u);
    EXPECT_EQ(back.tickets.at("erin").request_id, "req-1");
}

}  // namespace
}  // namespace matchmesh::node
