// Tests for MatchmakingEngine: queues per shard, lookups, cancellation and
// the background worker pool.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"

namespace matchmesh::core {
namespace {

using namespace std::chrono_literals;

// Waits up to `timeout` for `condition` to become true, checking every 5 ms.
// Used where background threads do the work, so we can't know the exact moment.
template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

// ---- Deterministic tests: no workers, time controlled by the test ----

class EngineTest : public ::testing::Test {
protected:
    EngineTest() : engine_("node-a", EngineConfig{.worker_count = 0}, [this] { return now_; }) {}

    void enqueue_four(int base_rating) {
        for (int i = 0; i < 4; ++i) {
            const auto outcome =
                engine_.enqueue({"p" + std::to_string(base_rating + i), base_rating + i},
                                "us-west", "ranked");
            ASSERT_EQ(outcome.result, EnqueueResult::kAdded);
        }
    }

    TimePoint now_{};  // only touched by the test thread; no workers are running
    MatchmakingEngine engine_;
    const QueueKey ranked2_ = make_queue_key("us-west", "ranked", 1200);
};

TEST_F(EngineTest, EnqueuePlacesPlayerInTheQueueForTheirBracket) {
    now_ += 3s;
    const auto outcome = engine_.enqueue({"alice", 1300}, "us-west", "ranked");

    EXPECT_EQ(outcome.result, EnqueueResult::kAdded);
    EXPECT_EQ(outcome.queue, make_queue_key("us-west", "ranked", 1300));
    EXPECT_EQ(outcome.enqueued_at, TimePoint{} + 3s);
    EXPECT_EQ(engine_.lookup(outcome.queue, "alice").status, PlayerStatus::kQueued);
    EXPECT_EQ(engine_.queued_players(), 1u);
}

TEST_F(EngineTest, RejectsDuplicatesAndEmptyIds) {
    ASSERT_EQ(engine_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);

    EXPECT_EQ(engine_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kDuplicate);
    EXPECT_EQ(engine_.enqueue({"", 1300}, "us-west", "ranked").result,
              EnqueueResult::kInvalidPlayer);
    EXPECT_EQ(engine_.queued_players(), 1u);
}

TEST_F(EngineTest, MatchPassFormsALobbyAndLookupFindsIt) {
    enqueue_four(1200);

    EXPECT_EQ(engine_.run_match_pass(now_), 1u);

    const PlayerLookup found = engine_.lookup(ranked2_, "p1202");
    ASSERT_EQ(found.status, PlayerStatus::kInLobby);
    ASSERT_TRUE(found.lobby.has_value());
    EXPECT_EQ(found.lobby->lobby_id, "node-a-1");
    EXPECT_EQ(found.lobby->players.size(), 4u);
    EXPECT_EQ(engine_.queued_players(), 0u);
    EXPECT_EQ(engine_.lobbies_formed(), 1u);
}

TEST_F(EngineTest, EveryLobbyMemberSeesTheSameLobby) {
    enqueue_four(1200);
    ASSERT_EQ(engine_.run_match_pass(now_), 1u);

    for (const char* id : {"p1200", "p1201", "p1202", "p1203"}) {
        const PlayerLookup found = engine_.lookup(ranked2_, id);
        ASSERT_EQ(found.status, PlayerStatus::kInLobby) << id;
        EXPECT_EQ(found.lobby->lobby_id, "node-a-1") << id;
    }
}

TEST_F(EngineTest, DifferentBracketsAndModesNeverMix) {
    // Two players in each of four different queues: no queue has four.
    for (const auto& [id, rating, mode] :
         {std::tuple{"a1", 1200, "ranked"}, {"a2", 1210, "ranked"},
          std::tuple{"b1", 1700, "ranked"}, {"b2", 1710, "ranked"},
          std::tuple{"c1", 1200, "casual"}, {"c2", 1210, "casual"},
          std::tuple{"d1", 1720, "casual"}, {"d2", 1730, "casual"}}) {
        ASSERT_EQ(engine_.enqueue({id, rating}, "us-west", mode).result, EnqueueResult::kAdded);
    }

    EXPECT_EQ(engine_.run_match_pass(now_ + 1h), 0u);
    EXPECT_EQ(engine_.queued_players(), 8u);
}

TEST_F(EngineTest, MatchPassUsesTheTimeItIsGiven) {
    // Spread 100: no lobby at first, one once players have waited 5 s.
    for (const auto& [id, rating] : {std::pair{"a", 1200}, {"b", 1240}, {"c", 1270}, {"d", 1300}}) {
        ASSERT_EQ(engine_.enqueue({id, rating}, "us-west", "ranked").result,
                  EnqueueResult::kAdded);
    }

    EXPECT_EQ(engine_.run_match_pass(now_), 0u);
    EXPECT_EQ(engine_.run_match_pass(now_ + 5s), 1u);
}

TEST_F(EngineTest, CancelRemovesAWaitingPlayer) {
    ASSERT_EQ(engine_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);

    EXPECT_EQ(engine_.cancel(ranked2_, "alice"), CancelResult::kCancelled);
    EXPECT_EQ(engine_.lookup(ranked2_, "alice").status, PlayerStatus::kNotFound);
    EXPECT_EQ(engine_.cancel(ranked2_, "alice"), CancelResult::kNotWaiting);
}

TEST_F(EngineTest, CancelInAnUnknownQueueDoesNothing) {
    EXPECT_EQ(engine_.cancel(make_queue_key("eu", "ranked", 1300), "alice"),
              CancelResult::kNotWaiting);
}

TEST_F(EngineTest, CannotCancelAfterBeingMatched) {
    enqueue_four(1200);
    ASSERT_EQ(engine_.run_match_pass(now_), 1u);

    EXPECT_EQ(engine_.cancel(ranked2_, "p1200"), CancelResult::kNotWaiting);
    EXPECT_EQ(engine_.lookup(ranked2_, "p1200").status, PlayerStatus::kInLobby);
}

TEST_F(EngineTest, LookupOfAnUnknownPlayerOrQueueIsNotFound) {
    EXPECT_EQ(engine_.lookup(ranked2_, "nobody").status, PlayerStatus::kNotFound);
    EXPECT_EQ(engine_.lookup(make_queue_key("eu", "casual", 100), "x").status,
              PlayerStatus::kNotFound);
}

TEST_F(EngineTest, RejoiningAfterALobbyStartsAFreshSearch) {
    enqueue_four(1200);
    ASSERT_EQ(engine_.run_match_pass(now_), 1u);

    ASSERT_EQ(engine_.enqueue({"p1200", 1200}, "us-west", "ranked").result,
              EnqueueResult::kAdded);

    const PlayerLookup found = engine_.lookup(ranked2_, "p1200");
    EXPECT_EQ(found.status, PlayerStatus::kQueued);
    EXPECT_FALSE(found.lobby.has_value());
}

TEST(EngineConfigTest, RejectsBadSettings) {
    EXPECT_THROW(MatchmakingEngine("x", EngineConfig{.worker_count = 2, .tick = 0ms}),
                 std::invalid_argument);
    EXPECT_THROW(MatchmakingEngine("x", EngineConfig{.match = {.lobby_size = 1}}),
                 std::invalid_argument);
    EXPECT_THROW(MatchmakingEngine("x", EngineConfig{}, nullptr), std::invalid_argument);
}

// ---- Worker pool tests: real threads, real clock ----

TEST(EngineWorkersTest, WorkersFormLobbiesInTheBackground) {
    MatchmakingEngine engine("node-a", EngineConfig{.worker_count = 2, .tick = 10ms});
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(engine.enqueue({"p" + std::to_string(i), 1200 + i}, "us-west", "ranked").result,
                  EnqueueResult::kAdded);
    }

    const QueueKey key = make_queue_key("us-west", "ranked", 1200);
    EXPECT_TRUE(eventually([&] { return engine.lookup(key, "p0").status == PlayerStatus::kInLobby; }));
}

TEST(EngineWorkersTest, WorkersEventuallyMatchAsWindowsWiden) {
    // Spread 100 is too wide at first. Nobody else enqueues, so only the
    // periodic tick can notice once the window has widened enough. (Fast
    // widening, 100 ms instead of 5 s, keeps the test quick.)
    MatchmakingEngine engine(
        "node-a", EngineConfig{.match = {.widen_per_second = 500}, .worker_count = 1, .tick = 10ms});
    for (const auto& [id, rating] : {std::pair{"a", 1200}, {"b", 1240}, {"c", 1270}, {"d", 1300}}) {
        ASSERT_EQ(engine.enqueue({id, rating}, "us-west", "ranked").result, EnqueueResult::kAdded);
    }

    EXPECT_EQ(engine.lobbies_formed(), 0u);
    EXPECT_TRUE(eventually([&] { return engine.lobbies_formed() == 1u; }));
}

TEST(EngineWorkersTest, ManyThreadsEnqueueingAcrossManyQueues) {
    // 4 client threads each fill 10 queues with one player; the 3 workers
    // must turn all 10 queues into exactly 10 lobbies, losing nobody.
    MatchmakingEngine engine("node-a", EngineConfig{.worker_count = 3, .tick = 10ms});
    constexpr int kQueues = 10;
    constexpr int kClients = 4;

    std::atomic<int> rejected{0};
    {
        std::vector<std::jthread> clients;
        for (int c = 0; c < kClients; ++c) {
            clients.emplace_back([&engine, &rejected, c] {
                for (int q = 0; q < kQueues; ++q) {
                    const Player player{"q" + std::to_string(q) + "-c" + std::to_string(c),
                                        1200 + c};
                    if (engine.enqueue(player, "region-" + std::to_string(q), "ranked").result !=
                        EnqueueResult::kAdded) {
                        ++rejected;
                    }
                }
            });
        }
    }  // jthreads join here

    EXPECT_EQ(rejected.load(), 0);
    EXPECT_TRUE(eventually([&] { return engine.lobbies_formed() == kQueues; }));
    EXPECT_EQ(engine.queued_players(), 0u);
}

TEST(EngineWorkersTest, ShutsDownPromptlyEvenWithALongTick) {
    // Workers sleep up to an hour between checks, but a stop request wakes
    // them immediately, so destruction must not take long.
    const auto start = std::chrono::steady_clock::now();
    {
        MatchmakingEngine engine("node-a", EngineConfig{.worker_count = 4, .tick = 1h});
        std::this_thread::sleep_for(20ms);  // let the workers reach their wait
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}

}  // namespace
}  // namespace matchmesh::core
