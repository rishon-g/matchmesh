// The simulated cluster behind the in-browser demo: the real core logic, a
// simulated network. These check that the demo shows what the real cluster
// does (failover without losing anyone, rejoining, split brain).

#include <string>

#include <gtest/gtest.h>

#include "matchmesh/sim/cluster.hpp"

namespace matchmesh::sim {
namespace {

constexpr double kSecond = 1000.0;

class SimClusterTest : public ::testing::Test {
protected:
    // Every player told "you're in" is still held somewhere, exactly once.
    void expect_safe(const Invariants& checks) {
        EXPECT_EQ(checks.lost, 0U);
        EXPECT_EQ(checks.in_two_lobbies, 0U);
        EXPECT_EQ(checks.acknowledged, checks.in_lobby + checks.queued);
    }

    Cluster cluster_{SimConfig{.arrivals_per_second = 40.0, .seed = 7}};
};

TEST_F(SimClusterTest, FormsAViewAndMatchesPlayers) {
    cluster_.advance(8 * kSecond);
    EXPECT_GE(cluster_.epoch(), 1U);
    EXPECT_EQ(cluster_.members().size(), 5U);
    const Invariants checks = cluster_.invariants();
    EXPECT_GT(checks.acknowledged, 200U);
    EXPECT_GT(checks.in_lobby, 100U);
    EXPECT_EQ(checks.contested_queues, 0U);
    expect_safe(checks);
}

TEST_F(SimClusterTest, AKilledNodesQueuesMoveToTheirBackupsWithEveryPlayer) {
    cluster_.advance(5 * kSecond);
    const auto before = cluster_.epoch();
    cluster_.kill("node-c");
    cluster_.advance(1 * kSecond);
    EXPECT_EQ(cluster_.epoch(), before) << "too soon: 1.5 s of silence first";
    cluster_.advance(2 * kSecond);
    EXPECT_GT(cluster_.epoch(), before);
    EXPECT_EQ(cluster_.members().size(), 4U);
    cluster_.advance(3 * kSecond);
    expect_safe(cluster_.invariants());
}

TEST_F(SimClusterTest, ARestartedNodeRejoinsAndGetsItsQueuesBack) {
    cluster_.advance(5 * kSecond);
    cluster_.kill("node-b");
    cluster_.advance(4 * kSecond);
    ASSERT_EQ(cluster_.members().size(), 4U);
    cluster_.restart("node-b");
    cluster_.advance(4 * kSecond);
    EXPECT_EQ(cluster_.members().size(), 5U);
    EXPECT_TRUE(cluster_.is_up("node-b"));
    expect_safe(cluster_.invariants());
}

TEST_F(SimClusterTest, LosingAQueuesPrimaryAndBackupTogetherIsTheOneUnsafeCase) {
    // Not an assertion that players are lost (that depends on who was
    // queued where), only that the cluster carries on with three nodes.
    cluster_.advance(5 * kSecond);
    cluster_.kill("node-a");
    cluster_.kill("node-b");
    cluster_.advance(5 * kSecond);
    EXPECT_EQ(cluster_.members().size(), 3U);
    const Invariants checks = cluster_.invariants();
    EXPECT_EQ(checks.in_two_lobbies, 0U);
    EXPECT_GT(checks.acknowledged, 0U);
}

TEST_F(SimClusterTest, ASplitServesTheSameQueuesTwiceUntilItHeals) {
    cluster_.advance(5 * kSecond);
    cluster_.split();
    cluster_.advance(5 * kSecond);
    EXPECT_GT(cluster_.invariants().contested_queues, 0U) << "split brain should show";

    cluster_.heal();
    cluster_.advance(5 * kSecond);
    EXPECT_EQ(cluster_.members().size(), 5U);
    EXPECT_EQ(cluster_.invariants().contested_queues, 0U);
}

TEST_F(SimClusterTest, TrafficCanBeStoppedAndRestarted) {
    cluster_.set_rate(0.0);
    cluster_.advance(5 * kSecond);
    EXPECT_EQ(cluster_.invariants().acknowledged, 0U);
    cluster_.set_rate(50.0);
    cluster_.advance(3 * kSecond);
    EXPECT_GT(cluster_.invariants().acknowledged, 0U);
}

TEST_F(SimClusterTest, ShortAdvancesAddUp) {
    // One display frame at 60 Hz is 16.7 ms: less than two 10 ms steps.
    for (int frame = 0; frame < 600; ++frame) {
        cluster_.advance(1000.0 / 60.0);
    }
    EXPECT_NEAR(cluster_.now_ms(), 10 * kSecond, 10.0);
}

TEST_F(SimClusterTest, SameSeedSameRun) {
    Cluster other{SimConfig{.arrivals_per_second = 40.0, .seed = 7}};
    cluster_.advance(4 * kSecond);
    other.advance(4 * kSecond);
    cluster_.kill("node-d");
    other.kill("node-d");
    cluster_.advance(3 * kSecond);
    other.advance(3 * kSecond);
    EXPECT_EQ(cluster_.snapshot_json(), other.snapshot_json());
}

TEST_F(SimClusterTest, SnapshotHasTheGatewayShapePlusTheDemoSection) {
    cluster_.advance(5 * kSecond);
    const std::string json = cluster_.snapshot_json();
    for (const char* field : {"\"epoch\":", "\"coordinator\":", "\"nodes\":[", "\"ring\":[",
                              "\"shards\":[", "\"lobbies\":[", "\"sim\":{", "\"invariants\":{",
                              "\"events\":["}) {
        EXPECT_NE(json.find(field), std::string::npos) << field;
    }
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

}  // namespace
}  // namespace matchmesh::sim
