#include "matchmesh/core/engine.hpp"

#include <stdexcept>
#include <utility>

namespace matchmesh::core {

MatchmakingEngine::MatchmakingEngine(std::string node_id, EngineConfig config, ClockFn clock)
    : config_(config), clock_(std::move(clock)), matcher_(std::move(node_id), config.match) {
    if (!clock_) {
        throw std::invalid_argument("clock must be callable");
    }
    if (config_.worker_count > 0 && config_.tick <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("tick must be positive when workers are running");
    }

    workers_.reserve(config_.worker_count);
    for (std::size_t i = 0; i < config_.worker_count; ++i) {
        // jthread passes its own stop_token as the first argument.
        workers_.emplace_back([this, i](std::stop_token stop) { worker_loop(std::move(stop), i); });
    }
}

MatchmakingEngine::~MatchmakingEngine() {
    // Ask every worker to stop first, then join them. (A jthread's own
    // destructor would stop and join them one at a time.)
    for (std::jthread& worker : workers_) {
        worker.request_stop();
    }
    workers_.clear();
}

EnqueueOutcome MatchmakingEngine::enqueue(Player player, std::string region,
                                          std::string game_mode) {
    QueueKey key = make_queue_key(std::move(region), std::move(game_mode), player.rating);
    if (player.id.empty()) {
        return {EnqueueResult::kInvalidPlayer, std::move(key), TimePoint{}};
    }

    EnqueueOutcome outcome{EnqueueResult::kAdded, key, TimePoint{}};
    {
        const std::scoped_lock lock(mutex_);
        auto& slot = shards_[key];
        if (!slot) {
            slot = std::make_unique<Shard>(std::move(key));
        }
        Shard& shard = *slot;

        outcome.enqueued_at = clock_();
        const std::string id = player.id;
        outcome.result = shard.queue.enqueue(std::move(player), outcome.enqueued_at);
        if (outcome.result != EnqueueResult::kAdded) {
            return outcome;
        }
        // Rejoining after a finished lobby starts a fresh search.
        shard.lobby_of.erase(id);
        ++enqueue_generation_;
    }
    // Notify after unlocking, so a woken worker doesn't immediately block on
    // the mutex we still hold.
    work_available_.notify_all();
    return outcome;
}

bool MatchmakingEngine::cancel(const QueueKey& queue, const std::string& player_id) {
    const std::scoped_lock lock(mutex_);
    const auto found = shards_.find(queue);
    if (found == shards_.end()) {
        return false;
    }
    return found->second->queue.remove(player_id).has_value();
}

PlayerLookup MatchmakingEngine::lookup(const QueueKey& queue, const std::string& player_id) const {
    const std::scoped_lock lock(mutex_);
    const auto found = shards_.find(queue);
    if (found == shards_.end()) {
        return {};
    }
    const Shard& shard = *found->second;
    if (shard.queue.contains(player_id)) {
        return {PlayerStatus::kQueued, std::nullopt};
    }
    if (const auto lobby = shard.lobby_of.find(player_id); lobby != shard.lobby_of.end()) {
        return {PlayerStatus::kInLobby, *lobby->second};
    }
    return {};
}

std::size_t MatchmakingEngine::run_match_pass(TimePoint now) {
    const std::scoped_lock lock(mutex_);
    return match_shards_locked(0, 1, now);
}

std::size_t MatchmakingEngine::queued_players() const {
    const std::scoped_lock lock(mutex_);
    std::size_t total = 0;
    for (const auto& [key, shard] : shards_) {
        total += shard->queue.size();
    }
    return total;
}

std::size_t MatchmakingEngine::lobbies_formed() const {
    const std::scoped_lock lock(mutex_);
    return lobbies_formed_;
}

std::size_t MatchmakingEngine::match_shards_locked(std::size_t index, std::size_t stride,
                                                   TimePoint now) {
    std::size_t formed = 0;
    for (auto& [key, shard] : shards_) {
        if (QueueKeyHash{}(key) % stride == index) {
            formed += match_shard_locked(*shard, now);
        }
    }
    return formed;
}

std::size_t MatchmakingEngine::match_shard_locked(Shard& shard, TimePoint now) {
    std::vector<Lobby> lobbies = matcher_.match(shard.queue, now);
    for (Lobby& lobby : lobbies) {
        // One shared copy per lobby; every member's entry points at it.
        auto shared = std::make_shared<const Lobby>(std::move(lobby));
        for (const Player& player : shared->players) {
            shard.lobby_of[player.id] = shared;
        }
    }
    lobbies_formed_ += lobbies.size();
    return lobbies.size();
}

void MatchmakingEngine::worker_loop(std::stop_token stop, std::size_t index) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        match_shards_locked(index, config_.worker_count, clock_());

        // Sleep until a new player arrives, the tick passes, or we're asked
        // to stop. wait_for releases the mutex while sleeping.
        const std::uint64_t seen = enqueue_generation_;
        work_available_.wait_for(lock, stop, config_.tick,
                                 [&] { return enqueue_generation_ != seen; });
    }
}

}  // namespace matchmesh::core
