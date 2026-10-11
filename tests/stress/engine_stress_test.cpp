// Stress tests: hammer one engine from many threads at once, then check that
// the result is still exactly right.
//
// These are meant to be run under the tsan preset. ThreadSanitizer watches
// every memory access while the threads collide, and reports any data race
// even if the final numbers happen to come out right.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"

namespace matchmesh::core {
namespace {

using namespace std::chrono_literals;

constexpr int kClientThreads = 8;
constexpr int kPlayersPerClient = 500;           // 4,000 players in total
const std::vector<std::string> kRegions = {"us-west", "us-east", "eu", "asia"};
const std::vector<std::string> kModes = {"ranked", "casual"};
// Ratings in two brackets (1000-1499 and 1500-1999), so 4 x 2 x 2 = 16 queues.
constexpr int kRatingLow = 1000;
constexpr int kRatingHigh = 1999;

// Everything the test needs to remember about one player.
struct PlayerRecord {
    std::string id;
    QueueKey queue;
    bool cancelled = false;  // a cancel() call returned true
};

// The engine's state as seen through lookup(), one player at a time.
struct Snapshot {
    std::size_t queued = 0;
    std::size_t in_lobby = 0;
    std::size_t not_found = 0;
    std::map<std::string, Lobby> lobbies;             // lobby_id -> lobby
    std::map<std::string, std::size_t> queued_per_queue;  // to_string(queue) -> count

    bool same_counts_as(const Snapshot& other) const {
        return queued == other.queued && in_lobby == other.in_lobby &&
               not_found == other.not_found && lobbies.size() == other.lobbies.size();
    }
};

Snapshot take_snapshot(const MatchmakingEngine& engine, const std::vector<PlayerRecord>& players) {
    Snapshot snap;
    for (const PlayerRecord& p : players) {
        const PlayerLookup found = engine.lookup(p.queue, p.id);
        switch (found.status) {
            case PlayerStatus::kQueued:
                ++snap.queued;
                ++snap.queued_per_queue[to_string(p.queue)];
                break;
            case PlayerStatus::kInLobby:
                ++snap.in_lobby;
                snap.lobbies.emplace(found.lobby->lobby_id, *found.lobby);
                break;
            case PlayerStatus::kNotFound:
                ++snap.not_found;
                break;
        }
    }
    return snap;
}

// Matching is effectively instant: any four players in the same queue fit
// (the window jumps straight to its maximum, which covers a whole bracket).
// That way, once the clients stop, the engine settles quickly into a state
// where no queue holds four or more players.
EngineConfig stress_config() {
    return EngineConfig{.match = {.initial_window = 500, .widen_per_second = 0, .max_window = 500},
                        .worker_count = 4,
                        .tick = 5ms};
}

TEST(EngineStressTest, ConcurrentEnqueueCancelAndLookupLoseNobody) {
    MatchmakingEngine engine("node-s", stress_config());

    // Each client thread owns its own slice of players, so every player is
    // enqueued exactly once. All of them still share the same 16 queues, so
    // the threads constantly collide on the same shards.
    std::vector<std::vector<PlayerRecord>> per_client(kClientThreads);
    std::atomic<int> unexpected_results{0};
    std::atomic<bool> clients_done{false};

    // Extra threads that keep reading (and matching) while the clients write.
    std::vector<std::jthread> noise;
    noise.emplace_back([&] {
        while (!clients_done) {
            (void)engine.queued_players();
            (void)engine.lobbies_formed();
        }
    });
    noise.emplace_back([&] {
        // A second matcher racing the background workers on the same shards.
        while (!clients_done) {
            engine.run_match_pass(Clock::now());
        }
    });

    {
        std::vector<std::jthread> clients;
        for (int c = 0; c < kClientThreads; ++c) {
            clients.emplace_back([&, c] {
                std::mt19937 rng(static_cast<std::mt19937::result_type>(c));  // reproducible
                std::uniform_int_distribution<int> rating(kRatingLow, kRatingHigh);
                std::uniform_int_distribution<std::size_t> region(0, kRegions.size() - 1);
                std::uniform_int_distribution<std::size_t> mode(0, kModes.size() - 1);
                std::uniform_int_distribution<int> percent(0, 99);

                auto& mine = per_client[static_cast<std::size_t>(c)];
                mine.reserve(kPlayersPerClient);
                for (int i = 0; i < kPlayersPerClient; ++i) {
                    const std::string id = "c" + std::to_string(c) + "-p" + std::to_string(i);
                    const EnqueueOutcome outcome = engine.enqueue(
                        {id, rating(rng)}, kRegions[region(rng)], kModes[mode(rng)]);
                    if (outcome.result != EnqueueResult::kAdded) {
                        ++unexpected_results;
                        continue;
                    }
                    PlayerRecord record{id, outcome.queue};

                    // Look ourselves up: we must be queued or already matched,
                    // never "not found".
                    if (engine.lookup(record.queue, id).status == PlayerStatus::kNotFound) {
                        ++unexpected_results;
                    }
                    // A quarter of players try to cancel. Whether that works
                    // depends on whether a worker matched them first, which
                    // is exactly the race we want to exercise.
                    if (percent(rng) < 25) {
                        record.cancelled = engine.cancel(record.queue, id);
                    }
                    mine.push_back(std::move(record));
                }
            });
        }
    }  // all client threads joined here
    clients_done = true;
    noise.clear();

    ASSERT_EQ(unexpected_results.load(), 0);

    std::vector<PlayerRecord> players;
    for (auto& slice : per_client) {
        players.insert(players.end(), slice.begin(), slice.end());
    }
    ASSERT_EQ(players.size(), static_cast<std::size_t>(kClientThreads * kPlayersPerClient));

    // Wait for the workers to finish matching everything they can. A worker
    // may finish a lobby while we're half-way through reading players, giving
    // a torn snapshot, so only trust it once two snapshots in a row agree and
    // no queue has four or more players left (no further match is possible).
    Snapshot snap;
    Snapshot previous;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    bool settled = false;
    while (!settled && std::chrono::steady_clock::now() < deadline) {
        previous = std::move(snap);
        snap = take_snapshot(engine, players);
        settled = snap.same_counts_as(previous);
        for (const auto& [queue, count] : snap.queued_per_queue) {
            settled = settled && count < 4;
        }
        if (!settled) {
            std::this_thread::sleep_for(10ms);
        }
    }
    ASSERT_TRUE(settled) << "the engine did not settle within 10 s";

    // 1. Nobody lost: every player is queued, in a lobby, or cancelled.
    std::size_t cancelled = 0;
    for (const PlayerRecord& p : players) {
        cancelled += p.cancelled ? 1 : 0;
    }
    EXPECT_EQ(snap.queued + snap.in_lobby + snap.not_found, players.size());
    EXPECT_EQ(snap.not_found, cancelled) << "only successfully cancelled players may be missing";

    // 2. A successful cancel is final: that player was never matched.
    for (const PlayerRecord& p : players) {
        if (p.cancelled) {
            EXPECT_EQ(engine.lookup(p.queue, p.id).status, PlayerStatus::kNotFound) << p.id;
        }
    }

    // 3. Lobbies are whole and consistent, and nobody is in two of them.
    std::map<std::string, std::string> lobby_of_player;
    for (const auto& [lobby_id, lobby] : snap.lobbies) {
        ASSERT_EQ(lobby.players.size(), 4u) << lobby_id;
        for (const Player& member : lobby.players) {
            const auto [it, inserted] = lobby_of_player.emplace(member.id, lobby_id);
            EXPECT_TRUE(inserted) << member.id << " is in " << it->second << " and " << lobby_id;
            EXPECT_EQ(skill_bracket_for(member.rating), lobby.queue.skill_bracket) << lobby_id;
        }
    }
    EXPECT_EQ(lobby_of_player.size(), snap.in_lobby);

    // 4. Every lobby ID is unique, and the engine's counter agrees.
    EXPECT_EQ(snap.lobbies.size(), engine.lobbies_formed());
    EXPECT_EQ(snap.in_lobby, 4 * engine.lobbies_formed());
    EXPECT_EQ(snap.queued, engine.queued_players());
}

TEST(EngineStressTest, ManyThreadsCreatingTheSameShardsAtOnce) {
    // Every thread enqueues into the same 50 brand-new queues at the same
    // moment, so they race to create each shard. Exactly one creation must
    // win per queue, and no player may be lost in the process.
    MatchmakingEngine engine("node-s", EngineConfig{.worker_count = 0});
    constexpr int kQueues = 50;

    std::atomic<int> failures{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kClientThreads; ++t) {
            threads.emplace_back([&engine, &failures, t] {
                for (int q = 0; q < kQueues; ++q) {
                    const Player player{"t" + std::to_string(t) + "-q" + std::to_string(q), 1200};
                    if (engine.enqueue(player, "region-" + std::to_string(q), "ranked").result !=
                        EnqueueResult::kAdded) {
                        ++failures;
                    }
                }
            });
        }
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(engine.queued_players(), static_cast<std::size_t>(kClientThreads * kQueues));
    // 8 players per queue, all in range: exactly two lobbies per queue.
    EXPECT_EQ(engine.run_match_pass(Clock::now()), static_cast<std::size_t>(2 * kQueues));
    EXPECT_EQ(engine.queued_players(), 0u);
}

TEST(EngineStressTest, ConcurrentMatchPassesNeverReuseALobbyId) {
    // Several threads run full match passes while others keep adding players,
    // so the shared lobby ID counter is hit from many threads at once. No
    // background workers, so the engine is completely still once these stop.
    EngineConfig config = stress_config();
    config.worker_count = 0;
    MatchmakingEngine engine("node-s", config);
    std::atomic<bool> done{false};
    std::atomic<std::size_t> formed_by_passes{0};

    std::vector<std::jthread> matchers;
    for (int m = 0; m < 3; ++m) {
        matchers.emplace_back([&] {
            while (!done) {
                formed_by_passes += engine.run_match_pass(Clock::now());
            }
        });
    }
    {
        std::vector<std::jthread> producers;
        for (int t = 0; t < 4; ++t) {
            producers.emplace_back([&engine, t] {
                for (int i = 0; i < 400; ++i) {
                    (void)engine.enqueue(
                        {"t" + std::to_string(t) + "-" + std::to_string(i), 1000 + (i % 500)},
                        "us-west", (i % 2 == 0) ? "ranked" : "casual");
                }
            });
        }
    }
    done = true;
    matchers.clear();

    // Collect every lobby by looking every player up.
    std::set<std::string> lobby_ids;
    std::size_t in_lobby = 0;
    for (int t = 0; t < 4; ++t) {
        for (int i = 0; i < 400; ++i) {
            const QueueKey key =
                make_queue_key("us-west", (i % 2 == 0) ? "ranked" : "casual", 1000 + (i % 500));
            const PlayerLookup found =
                engine.lookup(key, "t" + std::to_string(t) + "-" + std::to_string(i));
            if (found.status == PlayerStatus::kInLobby) {
                ++in_lobby;
                lobby_ids.insert(found.lobby->lobby_id);
            }
        }
    }
    // If two lobbies had shared an ID, the set would hold fewer IDs than
    // there are lobbies.
    EXPECT_GT(engine.lobbies_formed(), 0u);
    EXPECT_EQ(in_lobby, 4 * engine.lobbies_formed());
    EXPECT_EQ(lobby_ids.size(), engine.lobbies_formed()) << "two lobbies share an ID";
    EXPECT_EQ(formed_by_passes.load(), engine.lobbies_formed());
}

}  // namespace
}  // namespace matchmesh::core
