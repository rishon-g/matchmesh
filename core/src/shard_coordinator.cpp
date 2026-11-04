#include "matchmesh/core/shard_coordinator.hpp"

#include <utility>

namespace matchmesh::core {

// ---- BackupReplicator ----

BackupReplicator::BackupReplicator(const PlacementSource& placement, ReplicaTransport& transport)
    : placement_(placement), transport_(transport) {}

bool BackupReplicator::replicate(const ShardChange& change,
                                 const std::function<ShardState()>& current) {
    const Placement place = placement_.placement(change.queue);
    if (place.primary != placement_.self_id()) {
        return false;  // the queue has moved on; its new owner takes changes now
    }
    if (!place.backup) {
        return true;  // a one-node cluster: nobody to copy to
    }
    const std::string& backup = *place.backup;

    // A backup that may not hold a complete copy gets one first, as of just
    // before this change, so the change itself then fits.
    if (!is_synced(change.queue, backup, place.epoch)) {
        if (!transport_.send_snapshot(backup, place.epoch, current())) {
            return false;
        }
        mark_synced(change.queue, backup, place.epoch);
    }

    if (!transport_.send_change(backup, place.epoch, change)) {
        // We can't know whether the backup applied it (the reply may be what
        // got lost), so don't trust its copy: the next change sends a fresh
        // snapshot, which puts both copies back in step.
        forget(change.queue);
        return false;
    }
    return true;
}

bool BackupReplicator::sync(const QueueKey& queue, const std::function<ShardState()>& current) {
    const Placement place = placement_.placement(queue);
    if (place.primary != placement_.self_id()) {
        return false;
    }
    if (!place.backup) {
        return true;
    }
    if (is_synced(queue, *place.backup, place.epoch)) {
        return true;
    }
    if (!transport_.send_snapshot(*place.backup, place.epoch, current())) {
        return false;
    }
    mark_synced(queue, *place.backup, place.epoch);
    return true;
}

bool BackupReplicator::is_synced(const QueueKey& queue, const std::string& backup,
                                 std::uint64_t epoch) {
    const std::scoped_lock lock(mutex_);
    const auto it = synced_.find(queue);
    // Any epoch change re-sends the copy: the backup may have restarted or
    // dropped the queue in between, and epochs are rare.
    return it != synced_.end() && it->second.backup == backup && it->second.epoch == epoch;
}

void BackupReplicator::mark_synced(const QueueKey& queue, const std::string& backup,
                                   std::uint64_t epoch) {
    const std::scoped_lock lock(mutex_);
    synced_[queue] = SyncedCopy{backup, epoch};
}

void BackupReplicator::forget(const QueueKey& queue) {
    const std::scoped_lock lock(mutex_);
    synced_.erase(queue);
}

// ---- ShardCoordinator: progress ----

ShardCoordinator::ShardCoordinator(MatchmakingEngine& engine, const PlacementSource& placement,
                                   ReplicaTransport& transport, std::uint64_t incarnation)
    : engine_(engine), placement_(placement), transport_(transport), incarnation_(incarnation) {}

void ShardCoordinator::record_progress(const std::string& node,
                                       std::optional<std::uint64_t> synced_epoch) {
    const std::scoped_lock lock(mutex_);
    if (synced_epoch) {
        progress_[node] = *synced_epoch;
    } else {
        progress_.erase(node);  // e.g. a node that just restarted
    }
}

std::optional<std::uint64_t> ShardCoordinator::synced_epoch() const {
    const std::scoped_lock lock(mutex_);
    return synced_epoch_;
}

bool ShardCoordinator::settled(const MembershipView& view) const {
    const std::string& self = placement_.self_id();
    if (!view.lists(self, incarnation_)) {
        return false;  // a restarted process isn't a member until a view says so
    }
    const std::scoped_lock lock(mutex_);
    for (const std::string& member : view.members) {
        if (member == self) {
            if (synced_epoch_ != view.epoch) {
                return false;
            }
            continue;
        }
        const auto it = progress_.find(member);
        if (it == progress_.end() || it->second != view.epoch) {
            return false;
        }
    }
    return true;
}

// ---- Serving ----

std::optional<std::string> ShardCoordinator::admit(const QueueKey& queue) {
    const auto info = engine_.shard_info(queue);
    if (info && info->role == ShardRole::kPrimary) {
        return std::nullopt;  // ours, and live
    }
    const MembershipView view = placement_.view();
    if (!settled(view)) {
        return "queue " + to_string(queue) + " is moving to " + placement_.self_id() +
               " (epoch " + std::to_string(view.epoch) + "); retry";
    }
    if (info) {
        // A copy that was waiting for the hand-over to finish: it's ready.
        engine_.set_role(queue, ShardRole::kPrimary);
    }
    // No copy anywhere: a queue nobody has used yet starts here, empty.
    return std::nullopt;
}

// ---- Receiving ----

namespace {

ReplicaVerdict refused(std::uint64_t epoch, std::string reason) {
    return ReplicaVerdict{false, std::move(reason), epoch};
}

ReplicaVerdict accepted(std::uint64_t epoch) {
    return ReplicaVerdict{true, {}, epoch};
}

// Only the exact same epoch is accepted. Older: the sender is stale and may
// no longer own the queue, so it must not overwrite anything. Newer: this
// node hasn't caught up yet; the sender retries in a moment.
std::optional<ReplicaVerdict> wrong_epoch(std::uint64_t sent, std::uint64_t ours) {
    if (sent == ours) {
        return std::nullopt;
    }
    return refused(ours, sent < ours ? "sent under an older epoch"
                                     : "receiver hasn't seen that epoch yet; retry");
}

}  // namespace

ReplicaVerdict ShardCoordinator::receive(const std::string& sender, std::uint64_t epoch,
                                         const ShardChange& change) {
    // One lookup gives both who holds the queue and the epoch that's from,
    // so the checks below can't mix two different views.
    const Placement place = placement_.placement(change.queue);
    if (auto verdict = wrong_epoch(epoch, place.epoch)) {
        return *verdict;
    }
    const bool from_our_primary = place.backup == placement_.self_id() && sender == place.primary;
    if (!from_our_primary) {
        return refused(place.epoch,
                       "sender is not the queue's primary, or this node is not its backup");
    }
    if (!engine_.apply_replicated(change)) {
        return refused(place.epoch, "change " + std::to_string(change.sequence) +
                                        " doesn't follow this copy; send a snapshot");
    }
    return accepted(place.epoch);
}

ReplicaVerdict ShardCoordinator::receive(const std::string& sender, std::uint64_t epoch,
                                         const ShardState& state) {
    const Placement place = placement_.placement(state.queue);
    if (auto verdict = wrong_epoch(epoch, place.epoch)) {
        return *verdict;
    }
    const std::string& self = placement_.self_id();
    if (place.primary == self) {
        // A hand-over: this queue is ours (or becoming ours) now.
        const auto info = engine_.shard_info(state.queue);
        if (info && info->role == ShardRole::kPrimary) {
            return accepted(place.epoch);  // already live here; ours is authoritative
        }
        // Several nodes may hold a copy; keep the newest. It goes live once
        // every member has finished handing over (see admit).
        if (!info || state.sequence > info->sequence) {
            engine_.install(state, ShardRole::kReplica);
        }
        return accepted(place.epoch);
    }
    if (place.backup == self && sender == place.primary) {
        engine_.install(state, ShardRole::kReplica);  // a fresh backup copy
        return accepted(place.epoch);
    }
    return refused(place.epoch, "this node is neither the queue's owner nor its backup");
}

// ---- Reporting ----

std::vector<ServedShard> ShardCoordinator::served_shards() const {
    const std::string& self = placement_.self_id();
    std::vector<ServedShard> served;
    for (const QueueKey& queue : engine_.shards()) {
        const auto info = engine_.shard_info(queue);
        const Placement place = placement_.placement(queue);
        if (!info || info->role != ShardRole::kPrimary || place.primary != self) {
            continue;  // a replica, or on its way to another node
        }
        served.push_back(ServedShard{queue, place.backup, info->queued});
    }
    return served;
}

// ---- Handing over and taking over ----

CoordinatorPass ShardCoordinator::run_pass() {
    const MembershipView view = placement_.view();
    CoordinatorPass pass;
    pass.epoch = view.epoch;
    if (synced_epoch() != view.epoch && hand_over_all(view, pass.copies_sent)) {
        const std::scoped_lock lock(mutex_);
        synced_epoch_ = view.epoch;
        pass.finished_hand_over = true;
    }
    pass.promoted = promote_ready(placement_.view());
    sync_backups();
    return pass;
}

bool ShardCoordinator::hand_over_all(const MembershipView& view, std::size_t& sent) {
    const std::string& self = placement_.self_id();
    bool all_done = true;
    for (const QueueKey& queue : engine_.shards()) {
        const Placement place = placement_.placement(queue);
        if (place.epoch != view.epoch) {
            return false;  // a newer view arrived; start again under that one
        }
        if (place.primary == self) {
            continue;  // still ours (or waiting to be promoted)
        }
        const auto info = engine_.shard_info(queue);
        if (!info) {
            continue;
        }
        if (info->role == ShardRole::kPrimary) {
            // Stop changing it here first, so the copy we send is final.
            engine_.set_role(queue, ShardRole::kReplica);
        }
        const auto state = engine_.snapshot(queue);
        if (!state || !transport_.send_snapshot(place.primary, view.epoch, *state)) {
            all_done = false;  // retried on the next pass
            continue;
        }
        ++sent;
        if (place.backup != self) {
            engine_.drop(queue);  // lives elsewhere now
        }
    }
    return all_done && placement_.view().epoch == view.epoch;
}

std::size_t ShardCoordinator::promote_ready(const MembershipView& view) {
    if (!settled(view)) {
        return 0;
    }
    std::size_t promoted = 0;
    for (const QueueKey& queue : engine_.shards()) {
        const auto info = engine_.shard_info(queue);
        if (!info || info->role != ShardRole::kReplica) {
            continue;
        }
        const Placement place = placement_.placement(queue);
        if (place.epoch == view.epoch && place.primary == placement_.self_id()) {
            engine_.set_role(queue, ShardRole::kPrimary);
            ++promoted;
        }
    }
    return promoted;
}

void ShardCoordinator::sync_backups() {
    for (const QueueKey& queue : engine_.shards()) {
        if (placement_.placement(queue).primary == placement_.self_id()) {
            // Cheap when the backup is already in step (nothing is sent).
            (void)engine_.sync_backup(queue);
        }
    }
}

}  // namespace matchmesh::core
