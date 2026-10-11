#pragma once

// MatchmakingEngine: every queue a node owns, plus a fixed pool of worker
// threads that keep forming lobbies in the background.
//
// Each queue lives in its own *shard*. Shards are split between the workers
// by hashing the queue key, so every shard always has exactly one worker
// responsible for matching it.
//
// Thread-safe: gRPC handler threads call enqueue / cancel / lookup while the
// workers match. For now a single mutex guards all shards. That is simple
// and obviously correct, but it means a busy queue blocks every other queue;
// per-shard locks (lock striping) remove that bottleneck.
//
// Still no networking here, so the engine can be tested on its own,
// including under ThreadSanitizer.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "matchmesh/core/matcher.hpp"
#include "matchmesh/core/skill_queue.hpp"
#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

struct EngineConfig {
    MatchConfig match{};
    // Number of background worker threads. 0 starts none; call
    // run_match_pass() yourself instead (handy for deterministic tests).
    std::size_t worker_count = 4;
    // How often an idle worker re-checks its shards. Needed even with no new
    // players, because waiting players' windows keep widening.
    std::chrono::milliseconds tick{50};
};

enum class PlayerStatus {
    kQueued,    // still waiting in the queue
    kInLobby,   // placed in a lobby (see PlayerLookup::lobby)
    kNotFound,  // never joined this queue, or cancelled
};

struct PlayerLookup {
    PlayerStatus status = PlayerStatus::kNotFound;
    std::optional<Lobby> lobby;  // set only when status == kInLobby
};

struct EnqueueOutcome {
    EnqueueResult result;
    QueueKey queue;          // the queue the player was placed in (or would have been)
    TimePoint enqueued_at;   // meaningful only when result == kAdded
};

class MatchmakingEngine {
public:
    using ClockFn = std::function<TimePoint()>;

    // node_id prefixes lobby IDs so they are unique across the cluster.
    // `clock` lets tests control time; production uses the real clock.
    // Throws std::invalid_argument for a bad config.
    explicit MatchmakingEngine(std::string node_id, EngineConfig config = {},
                               ClockFn clock = Clock::now);

    // Stops the workers and waits for them to finish.
    ~MatchmakingEngine();

    MatchmakingEngine(const MatchmakingEngine&) = delete;
    MatchmakingEngine& operator=(const MatchmakingEngine&) = delete;

    // Puts a player in the queue for this region and game mode. The skill
    // bracket comes from their rating.
    EnqueueOutcome enqueue(Player player, std::string region, std::string game_mode);

    // Removes a waiting player. False if they weren't waiting in that queue.
    bool cancel(const QueueKey& queue, const std::string& player_id);

    // Where a player stands in one queue.
    [[nodiscard]] PlayerLookup lookup(const QueueKey& queue, const std::string& player_id) const;

    // Tries to match every shard once, as of `now`. Returns lobbies formed.
    // The workers do this automatically; tests call it directly.
    std::size_t run_match_pass(TimePoint now);

    [[nodiscard]] std::size_t queued_players() const;
    [[nodiscard]] std::size_t lobbies_formed() const;

private:
    // One queue plus the lobbies it has formed, so GetLobby can answer
    // "which lobby is this player in?".
    struct Shard {
        explicit Shard(QueueKey key) : queue(std::move(key)) {}

        SkillQueue queue;
        std::unordered_map<std::string, std::shared_ptr<const Lobby>> lobby_of;
    };

    // Match every shard whose hash % stride == index. Caller holds mutex_.
    std::size_t match_shards_locked(std::size_t index, std::size_t stride, TimePoint now);
    std::size_t match_shard_locked(Shard& shard, TimePoint now);

    void worker_loop(std::stop_token stop, std::size_t index);

    const EngineConfig config_;
    const ClockFn clock_;

    // ---- Everything below is guarded by mutex_ ----
    mutable std::mutex mutex_;
    // _any because it can wait on a std::stop_token: a stop request wakes a
    // sleeping worker immediately instead of after its tick.
    std::condition_variable_any work_available_;
    // unique_ptr keeps each Shard at a fixed address as the map grows.
    std::unordered_map<QueueKey, std::unique_ptr<Shard>, QueueKeyHash> shards_;
    Matcher matcher_;
    std::uint64_t enqueue_generation_ = 0;  // bumped on every enqueue to wake workers
    std::size_t lobbies_formed_ = 0;

    // Declared last so it is destroyed first: the workers are stopped and
    // joined before any state they use goes away.
    std::vector<std::jthread> workers_;
};

}  // namespace matchmesh::core
