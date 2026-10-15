#include "matchmesh/node/shard_manager.hpp"

#include <iostream>
#include <utility>
#include <vector>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {

ShardManager::ShardManager(core::MatchmakingEngine& engine, const Router& router,
                           ReplicationClient& replication, std::uint64_t incarnation,
                           ShardManagerConfig config)
    : engine_(engine),
      router_(router),
      replication_(replication),
      incarnation_(incarnation),
      config_(config) {}

ShardManager::~ShardManager() {
    stop();
}

void ShardManager::set_on_synced(std::function<void()> callback) {
    on_synced_ = std::move(callback);
}

void ShardManager::start() {
    if (!thread_.joinable()) {
        thread_ = std::jthread([this](std::stop_token stop) { loop(std::move(stop)); });
    }
}

void ShardManager::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void ShardManager::on_view_changed() {
    {
        const std::scoped_lock lock(wake_mutex_);
        ++generation_;
    }
    wake_.notify_all();
}

void ShardManager::record_progress(const std::string& node,
                                   std::optional<std::uint64_t> synced_epoch) {
    const std::scoped_lock lock(mutex_);
    if (synced_epoch) {
        progress_[node] = *synced_epoch;
    } else {
        progress_.erase(node);  // e.g. a node that just restarted
    }
}

std::optional<std::uint64_t> ShardManager::synced_epoch() const {
    const std::scoped_lock lock(mutex_);
    return synced_epoch_;
}

bool ShardManager::settled(const core::MembershipView& view) const {
    const std::string& self = router_.self_id();
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

std::optional<grpc::Status> ShardManager::admit(const core::QueueKey& queue) {
    const auto info = engine_.shard_info(queue);
    if (info && info->role == core::ShardRole::kPrimary) {
        return std::nullopt;  // ours, and live
    }
    const core::MembershipView view = router_.view();
    if (!settled(view)) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "queue " + core::to_string(queue) + " is moving to " +
                                router_.self_id() + " (epoch " + std::to_string(view.epoch) +
                                "); retry");
    }
    if (info) {
        // A copy that was waiting for the hand-over to finish: it's ready.
        engine_.set_role(queue, core::ShardRole::kPrimary);
    }
    // No copy anywhere: a queue nobody has used yet starts here, empty.
    return std::nullopt;
}

// ---- Receiving ----

void ShardManager::handle(const v1::ReplicateRequest& request, v1::ReplicateResponse* response) {
    const core::QueueKey queue = from_proto(request.queue());
    // One lookup gives both who holds the queue and the epoch that's from,
    // so the checks below can't mix two different views.
    const Placement place = router_.placement(queue);
    response->set_epoch(place.epoch);
    const auto reject = [response](std::string reason) {
        response->set_accepted(false);
        response->set_reason(std::move(reason));
    };
    const auto accept = [response] { response->set_accepted(true); };

    // Only the exact same epoch is accepted. Older: the sender is stale and
    // may no longer own the queue, so it must not overwrite anything. Newer:
    // this node hasn't caught up yet; the sender retries in a moment.
    if (request.epoch() != place.epoch) {
        reject(request.epoch() < place.epoch ? "sent under an older epoch"
                                             : "receiver hasn't seen that epoch yet; retry");
        return;
    }
    const std::string& self = router_.self_id();
    const bool from_our_primary = place.backup == self && request.sender_node_id() == place.primary;

    if (request.has_snapshot()) {
        const core::ShardState state = state_from_proto(request);
        if (place.primary == self) {
            // A hand-over: this queue is ours (or becoming ours) now.
            const auto info = engine_.shard_info(queue);
            if (info && info->role == core::ShardRole::kPrimary) {
                accept();  // already live here; our copy is the authoritative one
                return;
            }
            // Several nodes may hold a copy; keep the newest. It goes live
            // once every member has finished handing over (see admit).
            if (!info || state.sequence > info->sequence) {
                engine_.install(state, core::ShardRole::kReplica);
            }
            accept();
            return;
        }
        if (from_our_primary) {
            engine_.install(state, core::ShardRole::kReplica);  // a fresh backup copy
            accept();
            return;
        }
        reject("this node is neither the queue's owner nor its backup");
        return;
    }

    const std::optional<core::ShardChange> change = change_from_proto(request);
    if (!change) {
        reject("request carries no change");
        return;
    }
    if (!from_our_primary) {
        reject("sender is not the queue's primary, or this node is not its backup");
        return;
    }
    if (!engine_.apply_replicated(*change)) {
        reject("change " + std::to_string(change->sequence) +
               " doesn't follow this copy; send a snapshot");
        return;
    }
    accept();
}

// ---- Handing over and taking over ----

void ShardManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::uint64_t seen = 0;
        {
            const std::scoped_lock lock(wake_mutex_);
            seen = generation_;
        }

        const core::MembershipView view = router_.view();
        if (synced_epoch() != view.epoch) {
            std::size_t sent = 0;
            if (hand_over_all(view, sent)) {
                {
                    const std::scoped_lock lock(mutex_);
                    synced_epoch_ = view.epoch;
                }
                if (sent > 0) {
                    log("epoch " + std::to_string(view.epoch) + ": hand-over done, sent " +
                        std::to_string(sent) + " queue copy(s) to their owners");
                }
                if (on_synced_) {
                    on_synced_();
                }
            }
        }
        promote_ready(router_.view());
        sync_backups();

        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, stop, config_.retry_interval,
                       [&] { return generation_ != seen; });
    }
}

bool ShardManager::hand_over_all(const core::MembershipView& view, std::size_t& sent) {
    const std::string& self = router_.self_id();
    bool all_done = true;
    for (const core::QueueKey& queue : engine_.shards()) {
        const Placement place = router_.placement(queue);
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
        if (info->role == core::ShardRole::kPrimary) {
            // Stop changing it here first, so the copy we send is final.
            engine_.set_role(queue, core::ShardRole::kReplica);
        }
        const auto state = engine_.snapshot(queue);
        if (!state || !replication_.hand_over(place.primary, view.epoch, *state)) {
            all_done = false;  // retried on the next pass
            continue;
        }
        ++sent;
        if (place.backup != self) {
            engine_.drop(queue);  // lives elsewhere now
        }
    }
    return all_done && router_.epoch() == view.epoch;
}

void ShardManager::promote_ready(const core::MembershipView& view) {
    if (!settled(view)) {
        return;
    }
    std::size_t promoted = 0;
    for (const core::QueueKey& queue : engine_.shards()) {
        const auto info = engine_.shard_info(queue);
        if (!info || info->role != core::ShardRole::kReplica) {
            continue;
        }
        const Placement place = router_.placement(queue);
        if (place.epoch == view.epoch && place.primary == router_.self_id()) {
            engine_.set_role(queue, core::ShardRole::kPrimary);
            ++promoted;
        }
    }
    if (promoted > 0) {
        log("epoch " + std::to_string(view.epoch) + ": now serving " + std::to_string(promoted) +
            " more queue(s) as primary");
    }
}

void ShardManager::sync_backups() {
    for (const core::QueueKey& queue : engine_.shards()) {
        if (router_.placement(queue).primary == router_.self_id()) {
            // Cheap when the backup is already in step (nothing is sent).
            (void)engine_.sync_backup(queue);
        }
    }
}

void ShardManager::log(const std::string& message) const {
    if (!config_.log_changes) {
        return;
    }
    const std::scoped_lock lock(log_mutex_);
    std::cout << "[" << router_.self_id() << "] " << message << std::endl;
}

}  // namespace matchmesh::node
