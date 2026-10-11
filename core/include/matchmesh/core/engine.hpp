#pragma once

// MatchmakingEngine: every queue a node owns, plus a fixed pool of worker
// threads that keep forming lobbies in the background.
//
// Each queue lives in its own *shard*. Shards are split between the workers
// by hashing the queue key, so every shard always has exactly one worker
// responsible for matching it.
//
// Thread-safe, with lock striping: every shard has its own mutex, so work on
// one queue (a big match pass, a burst of enqueues) never blocks another
// queue. The locks are:
//
//   shards_mutex_   a reader/writer lock on the shard *map*. Finding an
//                   existing shard takes a shared (read) lock, so lookups
//                   never block each other; only creating a brand-new shard
//                   takes the exclusive (write) lock, and that happens once
//                   per queue.
//   Shard::mutex    guards one shard's queue and lobbies.
//   WorkerSignal    one small mutex + condition variable per worker, used
//                   only to wake that worker when its shards get a player.
//
// Lock ordering rule, which rules out deadlock: never hold two of these at
// once. Each operation finds its shard (map lock released), then locks just
// that shard.
//
// Still no networking here, so the engine can be tested on its own,
// including under ThreadSanitizer.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
    // `clock` lets tests control time; production uses the real clock. It may
    // be called from several threads at once.
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

    // Totals across all shards. Each shard is counted under its own lock, so
    // while other threads are busy the result is a close snapshot, not an
    // exact instant.
    [[nodiscard]] std::size_t queued_players() const;
    [[nodiscard]] std::size_t lobbies_formed() const noexcept;

private:
    // One queue plus the lobbies it has formed, so GetLobby can answer
    // "which lobby is this player in?".
    struct Shard {
        Shard(QueueKey key, std::size_t key_hash) : queue(std::move(key)), hash(key_hash) {}

        mutable std::mutex mutex;  // guards queue and lobby_of
        SkillQueue queue;
        std::unordered_map<std::string, std::shared_ptr<const Lobby>> lobby_of;
        const std::size_t hash;    // QueueKeyHash of the key, computed once
    };

    // Lets enqueue wake just the one worker that owns the shard it touched.
    struct WorkerSignal {
        std::mutex mutex;  // guards generation
        std::condition_variable_any wake;
        std::uint64_t generation = 0;  // bumped when this worker has new work
    };

    // Finds the shard for `key`, or nullptr. Never creates one.
    Shard* find_shard(const QueueKey& key) const;
    // Finds the shard for `key`, creating it if needed.
    Shard& find_or_create_shard(const QueueKey& key);
    // A snapshot of the shards to visit. stride 0 means all of them;
    // otherwise those whose hash % stride == index.
    std::vector<Shard*> shards_for(std::size_t index, std::size_t stride) const;

    std::size_t match_shard(Shard& shard, TimePoint now);
    void wake_owner_of(const Shard& shard);
    void worker_loop(std::stop_token stop, std::size_t index);

    const EngineConfig config_;
    const ClockFn clock_;
    Matcher matcher_;  // safe for concurrent use on different queues
    std::atomic<std::size_t> lobbies_formed_{0};

    mutable std::shared_mutex shards_mutex_;  // guards the map, not the shards
    // unique_ptr keeps each Shard at a fixed address as the map grows, and
    // shards are never removed, so a Shard* stays valid for the engine's life.
    std::unordered_map<QueueKey, std::unique_ptr<Shard>, QueueKeyHash> shards_;

    std::vector<std::unique_ptr<WorkerSignal>> signals_;  // one per worker
    // Declared last so it is destroyed first: the workers are stopped and
    // joined before any state they use goes away.
    std::vector<std::jthread> workers_;
};

}  // namespace matchmesh::core
