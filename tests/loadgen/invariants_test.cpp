// Tests for the lost-player invariant checker: a healthy run passes, and
// each kind of inconsistency the cluster could produce is caught.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/loadgen/invariants.hpp"

namespace matchmesh::loadgen {
namespace {

// A player in us-west/ranked at rating 1300 (all in one queue) by default.
PlayerResult player(const std::string& id, int rating = 1300) {
    PlayerResult result;
    result.player = PlannedPlayer{id, rating, "us-west", "ranked", {}};
    result.enqueued = true;
    result.status = FinalStatus::kQueued;
    return result;
}

PlayerResult in_lobby(const std::string& id, const std::string& lobby,
                      std::vector<std::string> members, int rating = 1300) {
    PlayerResult result = player(id, rating);
    result.status = FinalStatus::kInLobby;
    result.lobby_id = lobby;
    result.lobby_players = std::move(members);
    return result;
}

const std::vector<std::string> kABCD = {"a", "b", "c", "d"};

std::vector<PlayerResult> healthy() {
    return {in_lobby("a", "L1", kABCD), in_lobby("b", "L1", kABCD), in_lobby("c", "L1", kABCD),
            in_lobby("d", "L1", kABCD), player("e"), player("f")};
}

TEST(InvariantTest, AHealthyRunPasses) {
    const InvariantReport report = check_invariants(healthy());
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.players, 6u);
    EXPECT_EQ(report.in_one_lobby, 4u);
    EXPECT_EQ(report.queued, 2u);
    EXPECT_TRUE(report.problems.empty());
}

TEST(InvariantTest, APlayerTheClusterForgotIsLost) {
    auto players = healthy();
    players[4].status = FinalStatus::kNotFound;
    players[5].status = FinalStatus::kUnknown;  // nobody could even answer
    const InvariantReport report = check_invariants(players);
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.lost, 2u);
    EXPECT_EQ(report.queued, 0u);
}

TEST(InvariantTest, APlayerInTwoLobbiesIsCaught) {
    auto players = healthy();
    // e, f, g and a form a second lobby while a still reports the first.
    const std::vector<std::string> second = {"a", "e", "f", "g"};
    players[4] = in_lobby("e", "L2", second);
    players[5] = in_lobby("f", "L2", second);
    players.push_back(in_lobby("g", "L2", second));
    const InvariantReport report = check_invariants(players);
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.in_two_places, 1u);
    EXPECT_EQ(report.in_one_lobby, 6u);  // everyone but a
    ASSERT_FALSE(report.problems.empty());
    EXPECT_NE(report.problems.front().find("a is in lobby L2 and in lobby L1"), std::string::npos)
        << report.problems.front();
}

TEST(InvariantTest, APlayerInALobbyAndStillQueuedIsCaught) {
    auto players = healthy();
    players[3].status = FinalStatus::kQueued;  // d is listed in L1 but says "queued"
    players[3].lobby_id.clear();
    const InvariantReport report = check_invariants(players);
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.in_two_places, 1u);
}

TEST(InvariantTest, LobbiesOfTheWrongSizeAreCaught) {
    const std::vector<std::string> three = {"a", "b", "c"};
    const InvariantReport report = check_invariants(
        {in_lobby("a", "L1", three), in_lobby("b", "L1", three), in_lobby("c", "L1", three)});
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.bad_lobbies, 1u);
}

TEST(InvariantTest, MembersWhoDisagreeAboutTheirLobbyAreCaught) {
    auto players = healthy();
    players[3].lobby_players = {"a", "b", "c", "e"};
    const InvariantReport report = check_invariants(players);
    EXPECT_FALSE(report.ok());
    EXPECT_GE(report.bad_lobbies, 1u);
}

TEST(InvariantTest, ALobbyMixingQueuesIsCaught) {
    auto players = healthy();
    players[3] = in_lobby("d", "L1", kABCD, 2'200);  // a different skill bracket
    const InvariantReport report = check_invariants(players);
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.bad_lobbies, 1u);
}

TEST(InvariantTest, AStrangerInALobbyIsCaught) {
    const std::vector<std::string> members = {"a", "b", "c", "zed"};
    const InvariantReport report = check_invariants(
        {in_lobby("a", "L1", members), in_lobby("b", "L1", members), in_lobby("c", "L1", members)});
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.bad_lobbies, 1u);
}

TEST(InvariantTest, AFailedEnqueueIsReportedButOnlyAViolationIfThePlayerTurnsUp) {
    auto players = healthy();
    players[5].enqueued = false;  // f was told "failed" and is nowhere: fine
    players[5].status = FinalStatus::kNeverEnqueued;
    InvariantReport report = check_invariants(players);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.never_enqueued, 1u);

    players[3].enqueued = false;  // d was told "failed" but is in L1: a ghost
    report = check_invariants(players);
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.ghosts, 1u);
}

TEST(InvariantTest, OnlyTheFirstProblemsAreListed) {
    std::vector<PlayerResult> players;
    for (int i = 0; i < 50; ++i) {
        players.push_back(player("p" + std::to_string(i)));
        players.back().status = FinalStatus::kNotFound;
    }
    const InvariantReport report = check_invariants(players);
    EXPECT_EQ(report.lost, 50u);
    EXPECT_EQ(report.problems.size(), InvariantReport::kMaxProblems);
    EXPECT_EQ(report.problems_not_shown, 50u - InvariantReport::kMaxProblems);
}

}  // namespace
}  // namespace matchmesh::loadgen
