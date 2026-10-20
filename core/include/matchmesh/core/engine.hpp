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
//   Shard::write_mutex
//                   serialises *changes* to one shard (enqueue, cancel, a
//                   lobby forming, replicated changes, role changes). It is
//                   held while a change is copied to the backup, which is
//                   what keeps a shard's changes in order.
//   Shard::mutex    guards one shard's queue and lobbies. Held only briefly,
//                   never across a network call, so lookups stay fast even
//                   while a change is being replicated.
//   WorkerSignal    one small mutex + condition variable per worker, used
//                   only to wake that worker when its shards get a player.
//
// Lock ordering rule, which rules out deadlock: the map lock is never held
// with any other lock (find the shard, release it). The only nesting is a
// shard's write_mutex, then that same shard's mutex, always in that order.
//
// Replication (see replication.hpp): with a Replicator, every change is
// proposed, copied to the backup, and only then applied, so the backup is
// never behind what a player was told. Each shard is either the *primary*
// copy (matched by the workers, accepts client changes) or a *replica* kept
// for another node, which only changes when the primary says so.
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
#include "matchmesh/core/replication.hpp"
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

enum class CancelResult {
    kCancelled,
    kNotWaiting,      // the player wasn't waiting in that queue
    kNotPrimary,      // this node keeps only a replica of the queue
    kNotReplicated,   // the backup didn't confirm it, so nothing changed; safe to retry
};

enum class ShardRole {
    kPrimary,  // this node owns the queue: it is matched here and takes client changes
    kReplica,  // a copy kept for another node: changes only by replication
};

struct ShardInfo {
    ShardRole role = ShardRole::kPrimary;
    std::uint64_t sequence = 0;  // number of the last change applied
    std::size_t queued = 0;      // players waiting
};

struct EnqueueOutcome {
    EnqueueResult result;
    QueueKey queue;          // the queue the player was placed in (or would have been)
    TimePoint enqueued_at;   // when result is kAdded, or kAlreadyDone (the original time)
};

class MatchmakingEngine {
public:
    using ClockFn = std::function<TimePoint()>;

    // `lobby_id_prefix` starts every lobby ID, so IDs are unique across the
    // cluster: the node passes its ID plus its run's incarnation (see
    // node/identity.hpp), since a restarted node's counter starts again at 1.
    // `clock` lets tests control time; production uses the real clock. It may
    // be called from several threads at once. `replicator` (optional, must
    // outlive the engine) receives every change before it is applied;
    // without one the engine is a plain single-node engine.
    // Throws std::invalid_argument for a bad config.
    explicit MatchmakingEngine(std::string lobby_id_prefix, EngineConfig config = {},
                               ClockFn clock = Clock::now, Replicator* replicator = nullptr);

    // Stops the workers and waits for them to finish.
    ~MatchmakingEngine();

    MatchmakingEngine(const MatchmakingEngine&) = delete;
    MatchmakingEngine& operator=(const MatchmakingEngine&) = delete;

    // Puts a player in the queue for this region and game mode. The skill
    // bracket comes from their rating. A queue this node has never seen
    // starts here as a primary.
    //
    // `request_id` makes retries safe: if the player is still where an
    // earlier request with the same ID put them (queued, or in the lobby
    // they were matched into), nothing changes and the result is
    // kAlreadyDone with the original enqueued_at. A different ID (or none)
    // is a new join, so a player can queue again after a finished match.
    EnqueueOutcome enqueue(Player player, std::string region, std::string game_mode,
                           std::string request_id = {});

    // Removes a waiting player.
    CancelResult cancel(const QueueKey& queue, const std::string& player_id);

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

    // ---- Replication: replica side and moving shards between nodes ----

    // Every queue this node holds (as primary or replica).
    [[nodiscard]] std::vector<QueueKey> shards() const;
    // Role, sequence and size of one queue, or nothing if it isn't held.
    [[nodiscard]] std::optional<ShardInfo> shard_info(const QueueKey& queue) const;

    // Applies a change sent by the queue's primary to this node's replica,
    // creating the replica if it's new. Applied only if it is the next
    // change in sequence and fits the replica's state; otherwise nothing
    // changes and it returns false (the primary then sends a full copy).
    bool apply_replicated(const ShardChange& change);

    // Replaces a queue's entire state with `state`, in the given role.
    void install(const ShardState& state, ShardRole role);
    // A complete copy of a queue, or nothing if it isn't held.
    [[nodiscard]] std::optional<ShardState> snapshot(const QueueKey& queue) const;

    // Makes a held queue primary (the workers start matching it) or a
    // replica (they stop, and client changes are refused). Waits for a
    // change that is being replicated to finish first.
    void set_role(const QueueKey& queue, ShardRole role);
    // Forgets a queue entirely (it now lives on other nodes).
    void drop(const QueueKey& queue);

    // For a primary queue: has the replicator make sure the backup holds a
    // complete copy. False if it couldn't, or the queue isn't primary here.
    bool sync_backup(const QueueKey& queue);

private:
    // One queue plus the lobbies it has formed, so GetLobby can answer
    // "which lobby is this player in?".
    struct Shard {
        Shard(QueueKey key, std::size_t key_hash) : queue(std::move(key)), hash(key_hash) {}

        std::mutex write_mutex;    // serialises changes; see the lock rules above
        mutable std::mutex mutex;  // guards everything below
        SkillQueue queue;
        std::unordered_map<std::string, std::shared_ptr<const Lobby>> lobby_of;
        // Player -> the request that put them in the queue or their lobby.
        std::unordered_map<std::string, JoinTicket> tickets;
        // Empty when the queue isn't held here (never seen, or dropped).
        // Changed only with both locks held.
        std::optional<ShardRole> role;
        std::uint64_t sequence = 0;  // last change applied; both locks to change
        const std::size_t hash;      // QueueKeyHash of the key, computed once
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

    // Step 2 of every change: copy it to the backup. Call with the shard's
    // write lock held (and not its mutex). True without a replicator.
    bool replicate(Shard& shard, const ShardChange& change);
    // Step 3: apply a change. Checks it fits first and changes nothing if
    // not. Call with both of the shard's locks held.
    static bool apply_locked(Shard& shard, const ShardChange& change);
    // A full copy of a shard. Call with the shard's mutex held.
    static ShardState state_locked(const Shard& shard);
    // Empties a shard. Call with both of the shard's locks held.
    static void clear_locked(Shard& shard);
    void wake_owner_of(const Shard& shard);
    void worker_loop(std::stop_token stop, std::size_t index);

    const EngineConfig config_;
    const ClockFn clock_;
    Replicator* const replicator_;  // may be null
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
