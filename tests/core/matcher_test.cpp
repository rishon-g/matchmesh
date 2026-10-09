// Tests for Matcher: lobby formation, window widening, cancellation and
// edge cases.

#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/matcher.hpp"

namespace matchmesh::core {
namespace {

using namespace std::chrono_literals;

// Default config (window 50, +10 per second, max 300, lobbies of 4) and the
// us-west/ranked/2 queue (ratings 1000-1499). Time starts at t0_ and only
// moves when a test says so.
class MatcherTest : public ::testing::Test {
protected:
    // Adds players who all joined at `when`.
    void add(std::initializer_list<std::pair<const char*, int>> players, TimePoint when) {
        for (const auto& [id, rating] : players) {
            ASSERT_EQ(queue_.enqueue({id, rating}, when), EnqueueResult::kAdded) << id;
        }
    }

    static std::vector<std::string> ids_of(const Lobby& lobby) {
        std::vector<std::string> ids;
        for (const Player& player : lobby.players) {
            ids.push_back(player.id);
        }
        return ids;
    }

    const TimePoint t0_{};
    SkillQueue queue_{make_queue_key("us-west", "ranked", 1300)};
    Matcher matcher_{"node-a"};
};

// ---- Window widening ----

TEST_F(MatcherTest, WindowStartsAtTheInitialWidth) {
    EXPECT_EQ(matcher_.window_for(0s), 50);
}

TEST_F(MatcherTest, WindowWidensTenPointsPerSecondWaited) {
    EXPECT_EQ(matcher_.window_for(1s), 60);
    EXPECT_EQ(matcher_.window_for(5s), 100);
    EXPECT_EQ(matcher_.window_for(10s), 150);
}

TEST_F(MatcherTest, WindowWidensSmoothlyWithinASecond) {
    EXPECT_EQ(matcher_.window_for(500ms), 55);
}

TEST_F(MatcherTest, WindowStopsGrowingAtTheMaximum) {
    EXPECT_EQ(matcher_.window_for(25s), 300);
    EXPECT_EQ(matcher_.window_for(1h), 300);
    EXPECT_EQ(matcher_.window_for(std::chrono::hours{24 * 365}), 300);  // no overflow
}

TEST_F(MatcherTest, NegativeWaitIsTreatedAsNoWait) {
    EXPECT_EQ(matcher_.window_for(-3s), 50);
}

// ---- Edge cases: not enough players ----

TEST_F(MatcherTest, EmptyQueueFormsNoLobby) {
    EXPECT_TRUE(matcher_.match(queue_, t0_).empty());
    EXPECT_TRUE(matcher_.match(queue_, t0_ + 1h).empty());
}

TEST_F(MatcherTest, FewerThanFourPlayersNeverFormALobby) {
    add({{"a", 1300}, {"b", 1300}, {"c", 1300}}, t0_);

    EXPECT_TRUE(matcher_.match(queue_, t0_ + 1h).empty());
    EXPECT_EQ(queue_.size(), 3u);  // nobody was removed
}

// ---- Lobby formation ----

TEST_F(MatcherTest, FormsALobbyWhenSpreadFitsTheInitialWindow) {
    add({{"a", 1200}, {"b", 1210}, {"c", 1230}, {"d", 1250}}, t0_);

    const auto lobbies = matcher_.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 1u);
    const Lobby& lobby = lobbies.front();
    EXPECT_EQ(lobby.lobby_id, "node-a-1");
    EXPECT_EQ(lobby.queue, queue_.key());
    EXPECT_EQ(lobby.formed_at, t0_);
    EXPECT_EQ(ids_of(lobby), (std::vector<std::string>{"a", "b", "c", "d"}));
    EXPECT_TRUE(queue_.empty());  // matched players leave the queue
}

TEST_F(MatcherTest, LobbyPlayersAreOrderedByRating) {
    add({{"d", 1240}, {"b", 1210}, {"a", 1200}, {"c", 1220}}, t0_);

    const auto lobbies = matcher_.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(ids_of(lobbies.front()), (std::vector<std::string>{"a", "b", "c", "d"}));
}

TEST_F(MatcherTest, WaitsWhenSpreadIsTooWideThenMatchesOnceTheWindowWidens) {
    add({{"a", 1200}, {"b", 1240}, {"c", 1270}, {"d", 1300}}, t0_);  // spread 100

    EXPECT_TRUE(matcher_.match(queue_, t0_).empty());       // window 50
    EXPECT_TRUE(matcher_.match(queue_, t0_ + 4s).empty());  // window 90
    EXPECT_EQ(queue_.size(), 4u);

    const auto lobbies = matcher_.match(queue_, t0_ + 5s);  // window 100
    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(lobbies.front().formed_at, t0_ + 5s);
    EXPECT_TRUE(queue_.empty());
}

TEST_F(MatcherTest, SpreadExactlyEqualToTheWindowIsAccepted) {
    add({{"a", 1200}, {"b", 1220}, {"c", 1240}, {"d", 1250}}, t0_);  // spread 50

    EXPECT_EQ(matcher_.match(queue_, t0_).size(), 1u);
}

TEST_F(MatcherTest, NeverMatchesBeyondTheMaximumWindow) {
    add({{"a", 1000}, {"b", 1100}, {"c", 1200}, {"d", 1301}}, t0_);  // spread 301

    EXPECT_TRUE(matcher_.match(queue_, t0_ + 1h).empty());
    EXPECT_EQ(queue_.size(), 4u);
}

TEST_F(MatcherTest, PicksTheTightestGroupAndLeavesTheOutlierQueued) {
    add({{"a", 1000}, {"b", 1010}, {"c", 1020}, {"d", 1030}, {"outlier", 1400}}, t0_);

    const auto lobbies = matcher_.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(ids_of(lobbies.front()), (std::vector<std::string>{"a", "b", "c", "d"}));
    ASSERT_EQ(queue_.size(), 1u);
    EXPECT_EQ(queue_.begin()->player.id, "outlier");
}

TEST_F(MatcherTest, ChoosesTheTightestOfSeveralGroupsAroundTheAnchor) {
    // "bob" waited longest, so he is the anchor. Of the four runs of four
    // that include him, [1180, 1200, 1210, 1220] has the smallest spread.
    add({{"bob", 1200}}, t0_);
    add({{"a", 1000}, {"b", 1150}, {"c", 1180}, {"d", 1210}, {"e", 1220}, {"f", 1400}},
        t0_ + 1s);

    const auto lobbies = matcher_.match(queue_, t0_ + 1s);

    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(ids_of(lobbies.front()), (std::vector<std::string>{"c", "bob", "d", "e"}));
}

TEST_F(MatcherTest, FormsSeveralLobbiesInOneCall) {
    add({{"a1", 1000}, {"a2", 1010}, {"a3", 1020}, {"a4", 1030},
         {"b1", 1300}, {"b2", 1310}, {"b3", 1320}, {"b4", 1330}},
        t0_);

    const auto lobbies = matcher_.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 2u);
    EXPECT_TRUE(queue_.empty());
}

TEST_F(MatcherTest, LobbyIdsAreUniqueAndIncreaseAcrossCalls) {
    add({{"a1", 1000}, {"a2", 1010}, {"a3", 1020}, {"a4", 1030}}, t0_);
    const auto first = matcher_.match(queue_, t0_);
    add({{"b1", 1300}, {"b2", 1310}, {"b3", 1320}, {"b4", 1330}}, t0_);
    const auto second = matcher_.match(queue_, t0_);

    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first.front().lobby_id, "node-a-1");
    EXPECT_EQ(second.front().lobby_id, "node-a-2");
}

TEST_F(MatcherTest, NoPlayerEndsUpInTwoLobbies) {
    // 13 players in a tight band: three lobbies form and one player is left.
    add({{"p01", 1200}, {"p02", 1201}, {"p03", 1202}, {"p04", 1203}, {"p05", 1204},
         {"p06", 1205}, {"p07", 1206}, {"p08", 1207}, {"p09", 1208}, {"p10", 1209},
         {"p11", 1210}, {"p12", 1211}, {"p13", 1212}},
        t0_);

    const auto lobbies = matcher_.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 3u);
    std::set<std::string> seen;
    for (const Lobby& lobby : lobbies) {
        ASSERT_EQ(lobby.players.size(), 4u);
        for (const Player& player : lobby.players) {
            EXPECT_TRUE(seen.insert(player.id).second) << player.id << " is in two lobbies";
            EXPECT_FALSE(queue_.contains(player.id)) << player.id << " is still queued";
        }
    }
    EXPECT_EQ(seen.size(), 12u);
    EXPECT_EQ(queue_.size(), 1u);  // every player is matched or still waiting, never lost
}

// ---- Longest waiter decides ----

TEST_F(MatcherTest, LongestWaitingPlayersWindowDecides) {
    // Alice has waited 25 s (window 300). The newcomers have just arrived
    // (window 50). Their group with her has spread 220: too wide for the
    // newcomers, but within Alice's window, so the lobby forms.
    add({{"alice", 1000}}, t0_);
    add({{"n1", 1150}, {"n2", 1200}, {"n3", 1220}}, t0_ + 25s);

    const auto lobbies = matcher_.match(queue_, t0_ + 25s);

    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(ids_of(lobbies.front()), (std::vector<std::string>{"alice", "n1", "n2", "n3"}));
}

TEST_F(MatcherTest, LongestWaitingPlayerIsServedBeforeATighterNewcomerGroup) {
    // The starvation case. Four newcomers could form a tight lobby on their
    // own (spread 40), which would leave Alice alone again. Because the
    // longest waiter goes first, Alice's group forms instead and one
    // newcomer waits. Without this, a steady stream of newcomers could keep
    // her waiting forever.
    add({{"alice", 1000}}, t0_);
    add({{"n1", 1180}, {"n2", 1200}, {"n3", 1210}, {"n4", 1220}}, t0_ + 25s);

    const auto lobbies = matcher_.match(queue_, t0_ + 25s);

    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(ids_of(lobbies.front()), (std::vector<std::string>{"alice", "n1", "n2", "n3"}));
    ASSERT_EQ(queue_.size(), 1u);
    EXPECT_EQ(queue_.begin()->player.id, "n4");
}

TEST_F(MatcherTest, NewcomersAloneDoNotWidenTheWindow) {
    // Same group, but nobody has waited: spread 220 is too wide.
    add({{"alice", 1000}, {"n1", 1150}, {"n2", 1200}, {"n3", 1220}}, t0_);

    EXPECT_TRUE(matcher_.match(queue_, t0_).empty());
}

// ---- Cancellation ----

TEST_F(MatcherTest, CancelledPlayersAreNeverMatched) {
    add({{"a", 1200}, {"b", 1210}, {"c", 1220}, {"d", 1230}, {"e", 1240}}, t0_);
    ASSERT_TRUE(queue_.remove("b").has_value());

    const auto lobbies = matcher_.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 1u);
    EXPECT_EQ(ids_of(lobbies.front()), (std::vector<std::string>{"a", "c", "d", "e"}));
}

TEST_F(MatcherTest, CancellingBelowFourPlayersPreventsALobby) {
    add({{"a", 1200}, {"b", 1210}, {"c", 1220}, {"d", 1230}}, t0_);
    ASSERT_TRUE(queue_.remove("d").has_value());

    EXPECT_TRUE(matcher_.match(queue_, t0_ + 1h).empty());
    EXPECT_EQ(queue_.size(), 3u);
}

TEST_F(MatcherTest, MatchedPlayersCanNoLongerBeCancelled) {
    add({{"a", 1200}, {"b", 1210}, {"c", 1220}, {"d", 1230}}, t0_);
    ASSERT_EQ(matcher_.match(queue_, t0_).size(), 1u);

    EXPECT_FALSE(queue_.remove("a").has_value());
}

// ---- Configuration ----

TEST_F(MatcherTest, HonoursACustomLobbySize) {
    Matcher duos("node-b", MatchConfig{.lobby_size = 2});
    add({{"a", 1200}, {"b", 1210}, {"c", 1300}, {"d", 1310}, {"e", 1400}}, t0_);

    const auto lobbies = duos.match(queue_, t0_);

    ASSERT_EQ(lobbies.size(), 2u);
    EXPECT_EQ(lobbies[0].players.size(), 2u);
    EXPECT_EQ(lobbies[0].lobby_id, "node-b-1");
    EXPECT_EQ(queue_.size(), 1u);
}

TEST(MatcherConfigTest, RejectsNonsenseSettings) {
    EXPECT_THROW(Matcher("x", MatchConfig{.lobby_size = 1}), std::invalid_argument);
    EXPECT_THROW(Matcher("x", MatchConfig{.initial_window = -1}), std::invalid_argument);
    EXPECT_THROW(Matcher("x", MatchConfig{.widen_per_second = -5}), std::invalid_argument);
    EXPECT_THROW(Matcher("x", MatchConfig{.initial_window = 100, .max_window = 50}),
                 std::invalid_argument);
    EXPECT_NO_THROW(Matcher("x", MatchConfig{}));
}

}  // namespace
}  // namespace matchmesh::core
