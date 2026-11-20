// Tests for the core <-> protobuf translations in convert.hpp.

#include <chrono>
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

}  // namespace
}  // namespace matchmesh::node
