// Tests for the gateway's snapshot: merging every node's GetTopology answer
// into the one JSON document the dashboard draws.

#include <string>
#include <vector>

#include <crow/json.h>
#include <gtest/gtest.h>

#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/gateway/snapshot.hpp"

namespace matchmesh::gateway {
namespace {

v1::GetTopologyResponse view(std::uint64_t epoch, const std::vector<std::string>& alive,
                             const std::vector<std::string>& down = {}) {
    v1::GetTopologyResponse response;
    response.set_epoch(epoch);
    response.set_coordinator_node_id(alive.front());
    for (const auto& id : alive) {
        auto* node = response.add_nodes();
        node->set_node_id(id);
        node->set_alive(true);
    }
    for (const auto& id : down) {
        response.add_nodes()->set_node_id(id);
    }
    return response;
}

void add_shard(v1::GetTopologyResponse& response, const std::string& region,
               const std::string& primary, const std::string& backup, std::uint32_t queued) {
    auto* shard = response.add_shards();
    shard->mutable_queue()->set_region(region);
    shard->mutable_queue()->set_game_mode("ranked");
    shard->mutable_queue()->set_skill_bracket(2);
    shard->set_primary_node_id(primary);
    shard->set_backup_node_id(backup);
    shard->set_queued_players(queued);
}

void add_lobby(v1::GetTopologyResponse& response, const std::string& id, std::int64_t seconds) {
    auto* lobby = response.add_recent_lobbies();
    lobby->set_lobby_id(id);
    lobby->mutable_queue()->set_region("eu");
    lobby->mutable_queue()->set_game_mode("ranked");
    lobby->mutable_queue()->set_skill_bracket(2);
    lobby->mutable_formed_at()->set_seconds(seconds);
    for (int i = 0; i < 4; ++i) {
        auto* player = lobby->add_players();
        player->set_player_id(id + "-p" + std::to_string(i));
        player->set_rating(1300 + i);
    }
}

crow::json::rvalue parse(const std::string& json) {
    auto doc = crow::json::load(json);
    EXPECT_TRUE(doc) << json;
    return doc;
}

TEST(SnapshotTest, MembershipComesFromTheNewestViewAndUnreachableNodesAreMarked) {
    std::vector<NodePoll> polls = {
        {"node-a", "a:1", view(4, {"node-a", "node-b", "node-c"})},
        {"node-b", "b:1", view(5, {"node-a", "node-b"}, {"node-c"})},  // newer
        {"node-c", "c:1", std::nullopt},                               // didn't answer
    };
    const auto doc = parse(build_snapshot(polls));
    EXPECT_EQ(doc["epoch"].u(), 5u);
    EXPECT_EQ(doc["coordinator"].s(), "node-a");
    EXPECT_EQ(doc["members"].u(), 2u);
    ASSERT_EQ(doc["nodes"].size(), 3u);
    EXPECT_TRUE(doc["nodes"][0]["reachable"].b());
    EXPECT_FALSE(doc["nodes"][2]["reachable"].b());
    EXPECT_FALSE(doc["nodes"][2]["member"].b());
    EXPECT_EQ(doc["nodes"][2]["address"].s(), "c:1");
}

TEST(SnapshotTest, TheRingHasEveryMembersPointsInOrder) {
    const std::vector<NodePoll> polls = {{"node-a", "a:1", view(1, {"node-a", "node-b"})}};
    const auto doc = parse(build_snapshot(polls));
    ASSERT_EQ(doc["ring"].size(), 2 * core::HashRing::kDefaultVirtualNodes);
    double previous = -1;
    for (std::size_t i = 0; i < doc["ring"].size(); ++i) {
        const double at = doc["ring"][i]["at"].d();
        ASSERT_GE(at, 0.0);
        ASSERT_LT(at, 1.0);
        ASSERT_GT(at, previous);
        previous = at;
    }
}

TEST(SnapshotTest, ShardsFromAllNodesAreMergedCountedAndSortedRoundTheRing) {
    auto a = view(1, {"node-a", "node-b"});
    auto b = view(1, {"node-a", "node-b"});
    add_shard(a, "eu", "node-a", "node-b", 3);
    add_shard(a, "asia", "node-a", "node-b", 1);
    add_shard(b, "us-west", "node-b", "node-a", 7);
    const auto doc = parse(build_snapshot({{"node-a", "a:1", a}, {"node-b", "b:1", b}}));

    ASSERT_EQ(doc["shards"].size(), 3u);
    EXPECT_EQ(doc["queued_players"].u(), 11u);
    for (std::size_t i = 1; i < doc["shards"].size(); ++i) {
        EXPECT_LT(doc["shards"][i - 1]["at"].d(), doc["shards"][i]["at"].d());
    }
    EXPECT_EQ(doc["nodes"][0]["queues"].u(), 2u);
    EXPECT_EQ(doc["nodes"][0]["queued"].u(), 4u);
    EXPECT_EQ(doc["nodes"][1]["queued"].u(), 7u);
}

TEST(SnapshotTest, AQueueClaimedTwiceWhileMovingIsShownOnceUnderTheRingsOwner) {
    const std::vector<std::string> members = {"node-a", "node-b"};
    core::HashRing ring;
    ring.add_node("node-a");
    ring.add_node("node-b");
    v1::QueueKey key;
    key.set_region("eu");
    key.set_game_mode("ranked");
    key.set_skill_bracket(2);
    const std::string owner = *ring.owner(core::QueueKey{"eu", "ranked", 2});
    const std::string other = owner == "node-a" ? "node-b" : "node-a";

    auto from_owner = view(1, members);
    auto from_other = view(1, members);
    add_shard(from_owner, "eu", owner, other, 5);
    add_shard(from_other, "eu", other, owner, 9);  // a stale claim during a hand-over
    // Whichever order the answers come in, the ring's owner wins.
    for (const bool owner_first : {true, false}) {
        std::vector<NodePoll> polls = {{other, "x", from_other}, {owner, "y", from_owner}};
        if (owner_first) {
            std::swap(polls[0], polls[1]);
        }
        const auto doc = parse(build_snapshot(polls));
        ASSERT_EQ(doc["shards"].size(), 1u);
        EXPECT_EQ(doc["shards"][0]["primary"].s(), owner);
        EXPECT_EQ(doc["shards"][0]["queued"].u(), 5u);
    }
}

TEST(SnapshotTest, LobbiesAreNewestFirstOnceEachAndCapped) {
    auto a = view(1, {"node-a", "node-b"});
    auto b = view(1, {"node-a", "node-b"});
    add_lobby(a, "L-old", 100);
    add_lobby(a, "L-new", 300);
    add_lobby(b, "L-mid", 200);
    add_lobby(b, "L-new", 300);  // reported twice (e.g. just after a failover)
    const auto doc = parse(build_snapshot({{"node-a", "a:1", a}, {"node-b", "b:1", b}}, 2));

    ASSERT_EQ(doc["lobbies"].size(), 2u);
    EXPECT_EQ(doc["lobbies"][0]["id"].s(), "L-new");
    EXPECT_EQ(doc["lobbies"][0]["formed_at"].i(), 300'000);
    EXPECT_EQ(doc["lobbies"][1]["id"].s(), "L-mid");
    EXPECT_EQ(doc["lobbies"][1]["node"].s(), "node-b");
    EXPECT_EQ(doc["lobbies"][0]["players"].size(), 4u);
    EXPECT_EQ(doc["lobbies"][0]["queue"].s(), "eu/ranked/2");
}

TEST(SnapshotTest, NobodyAnsweringStillGivesAValidDocument) {
    const auto doc = parse(build_snapshot({{"node-a", "a:1", std::nullopt}}));
    EXPECT_EQ(doc["epoch"].u(), 0u);
    EXPECT_EQ(doc["members"].u(), 0u);
    EXPECT_EQ(doc["ring"].size(), 0u);
    EXPECT_FALSE(doc["nodes"][0]["reachable"].b());
}

TEST(SnapshotTest, TheSameAnswersGiveTheSameJson) {
    auto a = view(3, {"node-a", "node-b"});
    add_shard(a, "eu", "node-a", "node-b", 2);
    add_lobby(a, "L1", 50);
    const std::vector<NodePoll> polls = {{"node-a", "a:1", a}, {"node-b", "b:1", std::nullopt}};
    EXPECT_EQ(build_snapshot(polls), build_snapshot(polls));  // how the poller spots changes
}

}  // namespace
}  // namespace matchmesh::gateway
