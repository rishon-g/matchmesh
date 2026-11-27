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

}  // namespace
}  // namespace matchmesh::node
