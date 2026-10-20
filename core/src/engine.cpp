#include "matchmesh/core/engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace matchmesh::core {

MatchmakingEngine::MatchmakingEngine(std::string lobby_id_prefix, EngineConfig config, ClockFn clock,
                                     Replicator* replicator)
    : config_(config),
      clock_(std::move(clock)),
      replicator_(replicator),
      matcher_(std::move(lobby_id_prefix), config.match) {
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

// ---- Client changes: propose, replicate, apply ----

EnqueueOutcome MatchmakingEngine::enqueue(Player player, std::string region,
                                          std::string game_mode, std::string request_id) {
    QueueKey key = make_queue_key(std::move(region), std::move(game_mode), player.rating);
    if (player.id.empty()) {
        return {EnqueueResult::kInvalidPlayer, std::move(key), TimePoint{}};
    }

    Shard& shard = find_or_create_shard(key);
    EnqueueOutcome outcome{EnqueueResult::kAdded, std::move(key), TimePoint{}};
    const std::scoped_lock write(shard.write_mutex);

    // 1. Propose.
    ShardChange change;
    {
        const std::scoped_lock lock(shard.mutex);
        if (!shard.role) {
            shard.role = ShardRole::kPrimary;  // a queue new to this node starts here
        }
        if (*shard.role != ShardRole::kPrimary) {
            outcome.result = EnqueueResult::kNotPrimary;
            return outcome;
        }
        // A retry of a join that already worked: the player is still where
        // that request put them. Answer as the first attempt would have.
        if (!request_id.empty()) {
            const auto ticket = shard.tickets.find(player.id);
            if (ticket != shard.tickets.end() && ticket->second.request_id == request_id &&
                (shard.queue.contains(player.id) || shard.lobby_of.contains(player.id))) {
                outcome.result = EnqueueResult::kAlreadyDone;
                outcome.enqueued_at = ticket->second.enqueued_at;
                return outcome;
            }
        }
        if (shard.queue.contains(player.id)) {
            outcome.result = EnqueueResult::kDuplicate;
            return outcome;
        }
        outcome.enqueued_at = clock_();
        change = ShardChange{shard.queue.key(), shard.sequence + 1,
                             PlayerJoined{std::move(player), outcome.enqueued_at,
                                          std::move(request_id)}};
    }
    // 2. Copy to the backup.
    if (!replicate(shard, change)) {
        outcome.result = EnqueueResult::kNotReplicated;
        return outcome;
    }
    // 3. Apply.
    {
        const std::scoped_lock lock(shard.mutex);
        apply_locked(shard, change);
    }
    wake_owner_of(shard);
    return outcome;
}

CancelResult MatchmakingEngine::cancel(const QueueKey& queue, const std::string& player_id) {
    Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return CancelResult::kNotWaiting;
    }
    const std::scoped_lock write(shard->write_mutex);
    ShardChange change;
    {
        const std::scoped_lock lock(shard->mutex);
        if (shard->role && *shard->role != ShardRole::kPrimary) {
            return CancelResult::kNotPrimary;
        }
        if (!shard->queue.contains(player_id)) {
            return CancelResult::kNotWaiting;
        }
        change = ShardChange{queue, shard->sequence + 1, PlayerLeft{player_id}};
    }
    if (!replicate(*shard, change)) {
        return CancelResult::kNotReplicated;
    }
    const std::scoped_lock lock(shard->mutex);
    apply_locked(*shard, change);
    return CancelResult::kCancelled;
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
        if (shard->role == ShardRole::kPrimary) {
            total += shard->queue.size();
        }
    }
    return total;
}

std::size_t MatchmakingEngine::lobbies_formed() const noexcept {
    return lobbies_formed_.load(std::memory_order_relaxed);
}

// ---- Replication: replica side and moving shards ----

std::vector<QueueKey> MatchmakingEngine::shards() const {
    std::vector<QueueKey> held;
    for (const Shard* shard : shards_for(0, 0)) {
        const std::scoped_lock lock(shard->mutex);
        if (shard->role) {
            held.push_back(shard->queue.key());
        }
    }
    return held;
}

std::optional<ShardInfo> MatchmakingEngine::shard_info(const QueueKey& queue) const {
    const Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return std::nullopt;
    }
    const std::scoped_lock lock(shard->mutex);
    if (!shard->role) {
        return std::nullopt;
    }
    return ShardInfo{*shard->role, shard->sequence, shard->queue.size()};
}

bool MatchmakingEngine::apply_replicated(const ShardChange& change) {
    Shard& shard = find_or_create_shard(change.queue);
    const std::scoped_lock write(shard.write_mutex);
    const std::scoped_lock lock(shard.mutex);
    if (!shard.role) {
        shard.role = ShardRole::kReplica;  // a fresh replica: only change 1 fits
    }
    if (*shard.role != ShardRole::kReplica) {
        return false;  // the primary's copy only changes through its own path
    }
    return apply_locked(shard, change);
}

void MatchmakingEngine::install(const ShardState& state, ShardRole role) {
    Shard& shard = find_or_create_shard(state.queue);
    {
        const std::scoped_lock write(shard.write_mutex);
        const std::scoped_lock lock(shard.mutex);
        clear_locked(shard);
        for (const PlayerJoined& joined : state.waiting) {
            // In arrival order, so equal ratings keep their tie-break order.
            (void)shard.queue.enqueue(joined.player, joined.enqueued_at);
        }
        shard.tickets.insert(state.tickets.begin(), state.tickets.end());
        std::unordered_map<std::string, std::shared_ptr<const Lobby>> by_id;
        for (const Lobby& lobby : state.lobbies) {
            by_id.emplace(lobby.lobby_id, std::make_shared<const Lobby>(lobby));
        }
        for (const auto& [player_id, lobby_id] : state.lobby_of) {
            if (const auto found = by_id.find(lobby_id); found != by_id.end()) {
                shard.lobby_of.emplace(player_id, found->second);
            }
        }
        shard.sequence = state.sequence;
        shard.role = role;
    }
    if (role == ShardRole::kPrimary) {
        wake_owner_of(shard);
    }
}

std::optional<ShardState> MatchmakingEngine::snapshot(const QueueKey& queue) const {
    const Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return std::nullopt;
    }
    const std::scoped_lock lock(shard->mutex);
    if (!shard->role) {
        return std::nullopt;
    }
    return state_locked(*shard);
}

void MatchmakingEngine::set_role(const QueueKey& queue, ShardRole role) {
    Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return;
    }
    {
        const std::scoped_lock write(shard->write_mutex);
        const std::scoped_lock lock(shard->mutex);
        if (!shard->role) {
            return;  // not held; nothing to promote or demote
        }
        shard->role = role;
    }
    if (role == ShardRole::kPrimary) {
        wake_owner_of(*shard);  // its players may already be ready to match
    }
}

void MatchmakingEngine::drop(const QueueKey& queue) {
    Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return;
    }
    const std::scoped_lock write(shard->write_mutex);
    const std::scoped_lock lock(shard->mutex);
    clear_locked(*shard);
}

bool MatchmakingEngine::sync_backup(const QueueKey& queue) {
    Shard* shard = find_shard(queue);
    if (shard == nullptr) {
        return false;
    }
    const std::scoped_lock write(shard->write_mutex);
    {
        const std::scoped_lock lock(shard->mutex);
        if (shard->role != ShardRole::kPrimary) {
            return false;
        }
    }
    if (replicator_ == nullptr) {
        return true;
    }
    return replicator_->sync(queue, [shard] {
        const std::scoped_lock lock(shard->mutex);
        return state_locked(*shard);
    });
}

// ---- Internals ----

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
    // any shard. That keeps the "map lock alone" rule.
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
    const std::scoped_lock write(shard.write_mutex);
    std::size_t formed = 0;
    // One lobby at a time: each is proposed, copied to the backup, and only
    // then applied, as a single change.
    while (true) {
        ShardChange change;
        {
            const std::scoped_lock lock(shard.mutex);
            if (shard.role != ShardRole::kPrimary) {
                break;  // replicas are matched by their primary, not here
            }
            std::optional<Lobby> lobby = matcher_.propose(shard.queue, now);
            if (!lobby) {
                break;
            }
            change = ShardChange{shard.queue.key(), shard.sequence + 1,
                                 LobbyFormed{std::move(*lobby)}};
        }
        if (!replicate(shard, change)) {
            break;  // the players stay queued; the next pass tries again
        }
        const std::scoped_lock lock(shard.mutex);
        apply_locked(shard, change);
        ++formed;
    }
    lobbies_formed_.fetch_add(formed, std::memory_order_relaxed);
    return formed;
}

bool MatchmakingEngine::replicate(Shard& shard, const ShardChange& change) {
    if (replicator_ == nullptr) {
        return true;
    }
    return replicator_->replicate(change, [&shard] {
        const std::scoped_lock lock(shard.mutex);
        return state_locked(shard);
    });
}

bool MatchmakingEngine::apply_locked(Shard& shard, const ShardChange& change) {
    if (change.sequence != shard.sequence + 1 || change.queue != shard.queue.key()) {
        return false;  // a gap, a repeat, or the wrong queue
    }

    if (const auto* joined = std::get_if<PlayerJoined>(&change.op)) {
        if (shard.queue.enqueue(joined->player, joined->enqueued_at) != EnqueueResult::kAdded) {
            return false;
        }
        // Rejoining after a finished lobby starts a fresh search.
        shard.lobby_of.erase(joined->player.id);
        if (joined->request_id.empty()) {
            shard.tickets.erase(joined->player.id);
        } else {
            shard.tickets[joined->player.id] = JoinTicket{joined->request_id, joined->enqueued_at};
        }
    } else if (const auto* left = std::get_if<PlayerLeft>(&change.op)) {
        if (!shard.queue.remove(left->player_id)) {
            return false;
        }
        shard.tickets.erase(left->player_id);  // that join is undone
    } else {
        const Lobby& lobby = std::get<LobbyFormed>(change.op).lobby;
        // Check every player first, so the lobby is applied whole or not at all.
        const bool all_waiting = !lobby.players.empty() &&
                                 std::ranges::all_of(lobby.players, [&](const Player& player) {
                                     return shard.queue.contains(player.id);
                                 });
        if (lobby.lobby_id.empty() || !all_waiting) {
            return false;
        }
        // One shared copy per lobby; every member's entry points at it.
        const auto shared = std::make_shared<const Lobby>(lobby);
        for (const Player& player : lobby.players) {
            shard.queue.remove(player.id);
            shard.lobby_of[player.id] = shared;
        }
    }
    shard.sequence = change.sequence;
    return true;
}

ShardState MatchmakingEngine::state_locked(const Shard& shard) {
    ShardState state{shard.queue.key(), shard.sequence, {}, {}, {}, {}};

    std::vector<const QueuedPlayer*> waiting;
    waiting.reserve(shard.queue.size());
    for (const QueuedPlayer& entry : shard.queue) {
        waiting.push_back(&entry);
    }
    std::ranges::sort(waiting, {}, &QueuedPlayer::arrival);
    state.waiting.reserve(waiting.size());
    for (const QueuedPlayer* entry : waiting) {
        const auto ticket = shard.tickets.find(entry->player.id);
        state.waiting.push_back(PlayerJoined{
            entry->player, entry->enqueued_at,
            ticket == shard.tickets.end() ? std::string{} : ticket->second.request_id});
    }
    state.tickets.insert(shard.tickets.begin(), shard.tickets.end());

    std::unordered_set<const Lobby*> seen;
    for (const auto& [player_id, lobby] : shard.lobby_of) {
        state.lobby_of.emplace(player_id, lobby->lobby_id);
        if (seen.insert(lobby.get()).second) {
            state.lobbies.push_back(*lobby);
        }
    }
    std::ranges::sort(state.lobbies, {}, &Lobby::lobby_id);  // deterministic order
    return state;
}

void MatchmakingEngine::clear_locked(Shard& shard) {
    shard.queue.clear();
    shard.lobby_of.clear();
    shard.tickets.clear();
    shard.sequence = 0;
    shard.role.reset();
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
