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

    signals_.reserve(config_.worker_count);
    workers_.reserve(config_.worker_count);
    for (std::size_t i = 0; i < config_.worker_count; ++i) {
        signals_.push_back(std::make_unique<WorkerSignal>());
    }
    // Create every signal before starting any thread: signals_ must not
    // change size once workers (and enqueues) may be reading it.
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

    Shard& shard = find_or_create_shard(key);
    EnqueueOutcome outcome{EnqueueResult::kAdded, std::move(key), TimePoint{}};
    {
        const std::scoped_lock lock(shard.mutex);  // this queue only
        outcome.enqueued_at = clock_();
        const std::string id = player.id;
        outcome.result = shard.queue.enqueue(std::move(player), outcome.enqueued_at);
        if (outcome.result != EnqueueResult::kAdded) {
            return outcome;
        }
        // Rejoining after a finished lobby starts a fresh search.
        shard.lobby_of.erase(id);
    }
    wake_owner_of(shard);  // after unlocking, so the worker doesn't wait on us
    return outcome;
}

bool MatchmakingEngine::cancel(const QueueKey& queue, const std::string& player_id) {
    Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return false;
    }
    const std::scoped_lock lock(shard->mutex);
    return shard->queue.remove(player_id).has_value();
}

PlayerLookup MatchmakingEngine::lookup(const QueueKey& queue, const std::string& player_id) const {
    const Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return {};
    }
    const std::scoped_lock lock(shard->mutex);
    if (shard->queue.contains(player_id)) {
        return {PlayerStatus::kQueued, std::nullopt};
    }
    if (const auto lobby = shard->lobby_of.find(player_id); lobby != shard->lobby_of.end()) {
        return {PlayerStatus::kInLobby, *lobby->second};
    }
    return {};
}

std::size_t MatchmakingEngine::run_match_pass(TimePoint now) {
    std::size_t formed = 0;
    for (Shard* shard : shards_for(0, 0)) {
        formed += match_shard(*shard, now);
    }
    return formed;
}

std::size_t MatchmakingEngine::queued_players() const {
    std::size_t total = 0;
    for (const Shard* shard : shards_for(0, 0)) {
        const std::scoped_lock lock(shard->mutex);
        total += shard->queue.size();
    }
    return total;
}

std::size_t MatchmakingEngine::lobbies_formed() const noexcept {
    return lobbies_formed_.load(std::memory_order_relaxed);
}

MatchmakingEngine::Shard* MatchmakingEngine::find_shard(const QueueKey& key) const {
    const std::shared_lock lock(shards_mutex_);  // many readers at once
    const auto found = shards_.find(key);
    return found == shards_.end() ? nullptr : found->second.get();
}

MatchmakingEngine::Shard& MatchmakingEngine::find_or_create_shard(const QueueKey& key) {
    // Fast path: the shard almost always exists already.
    if (Shard* existing = find_shard(key)) {
        return *existing;
    }
    // Slow path, once per queue: take the exclusive lock to add it.
    // try_emplace does nothing if another thread created it in the meantime.
    const std::unique_lock lock(shards_mutex_);
    auto [it, inserted] = shards_.try_emplace(key);
    if (inserted) {
        it->second = std::make_unique<Shard>(key, QueueKeyHash{}(key));
    }
    return *it->second;
}

std::vector<MatchmakingEngine::Shard*> MatchmakingEngine::shards_for(std::size_t index,
                                                                     std::size_t stride) const {
    // Copy the pointers out so we can release the map lock before touching
    // any shard. That keeps the "never hold two locks" rule.
    std::vector<Shard*> selected;
    const std::shared_lock lock(shards_mutex_);
    selected.reserve(stride == 0 ? shards_.size() : shards_.size() / stride + 1);
    for (const auto& [key, shard] : shards_) {
        if (stride == 0 || shard->hash % stride == index) {
            selected.push_back(shard.get());
        }
    }
    return selected;
}

std::size_t MatchmakingEngine::match_shard(Shard& shard, TimePoint now) {
    const std::scoped_lock lock(shard.mutex);
    std::vector<Lobby> lobbies = matcher_.match(shard.queue, now);
    for (Lobby& lobby : lobbies) {
        // One shared copy per lobby; every member's entry points at it.
        auto shared = std::make_shared<const Lobby>(std::move(lobby));
        for (const Player& player : shared->players) {
            shard.lobby_of[player.id] = shared;
        }
    }
    lobbies_formed_.fetch_add(lobbies.size(), std::memory_order_relaxed);
    return lobbies.size();
}

void MatchmakingEngine::wake_owner_of(const Shard& shard) {
    if (signals_.empty()) {
        return;  // no workers; tests call run_match_pass instead
    }
    WorkerSignal& signal = *signals_[shard.hash % signals_.size()];
    {
        const std::scoped_lock lock(signal.mutex);
        ++signal.generation;
    }
    signal.wake.notify_one();  // only the one worker that owns this shard
}

void MatchmakingEngine::worker_loop(std::stop_token stop, std::size_t index) {
    WorkerSignal& signal = *signals_[index];
    while (!stop.stop_requested()) {
        // Note the generation *before* matching. If a player arrives while we
        // match, the generation will have moved on and the wait below returns
        // straight away instead of sleeping through it.
        std::uint64_t seen = 0;
        {
            const std::scoped_lock lock(signal.mutex);
            seen = signal.generation;
        }

        const TimePoint now = clock_();
        for (Shard* shard : shards_for(index, config_.worker_count)) {
            match_shard(*shard, now);
        }

        // Sleep until a new player arrives in one of our shards, the tick
        // passes, or we're asked to stop.
        std::unique_lock lock(signal.mutex);
        signal.wake.wait_for(lock, stop, config_.tick, [&] { return signal.generation != seen; });
    }
}

}  // namespace matchmesh::core
