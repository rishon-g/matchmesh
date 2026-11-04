#include "matchmesh/node/shard_manager.hpp"

#include <iostream>
#include <utility>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {

ShardManager::ShardManager(core::MatchmakingEngine& engine, const Router& router,
                           ReplicationClient& replication, std::uint64_t incarnation,
                           ShardManagerConfig config)
    : engine_(engine),
      router_(router),
      config_(config),
      coordinator_(engine, router, replication, incarnation) {}

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
    coordinator_.record_progress(node, synced_epoch);
}

std::optional<std::uint64_t> ShardManager::synced_epoch() const {
    return coordinator_.synced_epoch();
}

bool ShardManager::settled(const core::MembershipView& view) const {
    return coordinator_.settled(view);
}

std::optional<grpc::Status> ShardManager::admit(const core::QueueKey& queue) {
    if (auto reason = coordinator_.admit(queue)) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, *reason);
    }
    return std::nullopt;
}

void ShardManager::handle(const v1::ReplicateRequest& request, v1::ReplicateResponse* response) {
    core::ReplicaVerdict verdict;
    if (request.has_snapshot()) {
        verdict = coordinator_.receive(request.sender_node_id(), request.epoch(),
                                       state_from_proto(request));
    } else if (const std::optional<core::ShardChange> change = change_from_proto(request)) {
        verdict = coordinator_.receive(request.sender_node_id(), request.epoch(), *change);
    } else {
        verdict = core::ReplicaVerdict{false, "request carries no change",
                                       router_.placement(from_proto(request.queue())).epoch};
    }
    response->set_epoch(verdict.epoch);
    response->set_accepted(verdict.accepted);
    if (!verdict.accepted) {
        response->set_reason(verdict.reason);
    }
}

void ShardManager::fill_topology(v1::GetTopologyResponse* response) const {
    for (const core::ServedShard& served : coordinator_.served_shards()) {
        v1::ShardAssignment* shard = response->add_shards();
        to_proto(served.queue, shard->mutable_queue());
        shard->set_primary_node_id(router_.self_id());
        shard->set_backup_node_id(served.backup.value_or(""));
        shard->set_queued_players(static_cast<std::uint32_t>(served.queued));
    }
    for (const core::Lobby& lobby : engine_.recent_lobbies()) {
        to_proto(lobby, response->add_recent_lobbies());
    }
}

void ShardManager::loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::uint64_t seen = 0;
        {
            const std::scoped_lock lock(wake_mutex_);
            seen = generation_;
        }

        const core::CoordinatorPass pass = coordinator_.run_pass();
        const std::string epoch = "epoch " + std::to_string(pass.epoch);
        if (pass.finished_hand_over) {
            if (pass.copies_sent > 0) {
                log(epoch + ": hand-over done, sent " + std::to_string(pass.copies_sent) +
                    " queue copy(s) to their owners");
            }
            if (on_synced_) {
                on_synced_();
            }
        }
        if (pass.promoted > 0) {
            log(epoch + ": now serving " + std::to_string(pass.promoted) +
                " more queue(s) as primary");
        }

        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, stop, config_.retry_interval,
                       [&] { return generation_ != seen; });
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
