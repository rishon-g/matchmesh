#include "matchmesh/node/cluster_manager.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>
#include <utility>

#include <grpcpp/grpcpp.h>

namespace matchmesh::node {
namespace {

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

template <typename Message>
core::MembershipView view_from(const Message& message) {
    core::MembershipView view{message.epoch(),
                              {message.members().begin(), message.members().end()},
                              {}};
    for (const auto& [node, incarnation] : message.incarnations()) {
        if (incarnation != 0) {
            view.incarnations.emplace(node, incarnation);
        }
    }
    return view;
}

template <typename Message>
void put_view(const core::MembershipView& view, Message* message) {
    message->set_epoch(view.epoch);
    for (const std::string& member : view.members) {
        message->add_members(member);
    }
    for (const auto& [node, incarnation] : view.incarnations) {
        (*message->mutable_incarnations())[node] = incarnation;
    }
}

}  // namespace

ClusterManager::ClusterManager(core::Membership& membership, Router& router,
                               std::string self_address, ClusterManagerConfig config,
                               ShardManager* shards)
    : membership_(membership),
      router_(router),
      self_address_(std::move(self_address)),
      config_(config),
      shards_(shards) {
    for (const auto& [id, address] : router_.peers()) {
        stubs_.emplace(id, v1::ClusterService::NewStub(make_peer_channel(address)));
    }
    if (shards_ != nullptr) {
        // Tell everyone as soon as our hand-over progress changes.
        shards_->set_on_synced([this] { nudge(); });
    }
}

ClusterManager::~ClusterManager() {
    stop();
}

void ClusterManager::start() {
    if (!threads_.empty()) {
        return;
    }
    for (const auto& [peer, stub] : stubs_) {
        threads_.emplace_back(
            [this, peer](std::stop_token stop) { heartbeat_loop(std::move(stop), peer); });
    }
    threads_.emplace_back([this](std::stop_token stop) { check_loop(std::move(stop)); });
}

void ClusterManager::stop() {
    for (std::jthread& thread : threads_) {
        thread.request_stop();
    }
    threads_.clear();  // joins
}

void ClusterManager::heartbeat_loop(std::stop_token stop, const std::string& peer) {
    v1::ClusterService::Stub& stub = *stubs_.at(peer);
    while (!stop.stop_requested()) {
        std::uint64_t seen = 0;
        {
            const std::scoped_lock lock(wake_mutex_);
            seen = view_generation_;
        }

        v1::HeartbeatRequest request;
        describe_self(&request);

        grpc::ClientContext context;
        // Fail fast if the peer is down; don't wait for it to come up.
        context.set_deadline(std::chrono::system_clock::now() + config_.heartbeat_timeout);
        v1::HeartbeatResponse response;
        if (stub.Heartbeat(&context, request, &response).ok()) {
            heard_from(peer, response);
        }

        // Sleep until the next beat, or until our view changes (so we pass
        // the news on right away), or until we're stopped.
        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, stop, config_.heartbeat_interval,
                       [&] { return view_generation_ != seen; });
    }
}

void ClusterManager::check_loop(std::stop_token stop) {
    std::vector<std::string> previously_alive = membership_.alive(core::Clock::now());
    while (!stop.stop_requested()) {
        const core::TimePoint now = core::Clock::now();

        // Log this node's own opinion changing (not yet the agreed view).
        const std::vector<std::string> alive = membership_.alive(now);
        for (const std::string& node : previously_alive) {
            if (!std::ranges::binary_search(alive, node)) {
                log(node + " stopped responding (no heartbeat for " +
                    std::to_string(membership_.config().suspect_after.count()) + " ms)");
            }
        }
        for (const std::string& node : alive) {
            if (!std::ranges::binary_search(previously_alive, node)) {
                log(node + " is responding again");
            }
        }
        previously_alive = alive;

        // If we're the coordinator and the alive set changed, publish.
        if (const auto published = membership_.tick(now)) {
            adopt(*published, "published as coordinator");
        }

        std::unique_lock lock(wake_mutex_);
        wake_.wait_for(lock, stop, config_.check_interval, [] { return false; });
    }
}

void ClusterManager::adopt(const core::MembershipView& view, std::string_view reason) {
    if (!router_.apply_view(view)) {
        return;  // the router already had this view (or a newer one)
    }
    log("epoch " + std::to_string(view.epoch) + ": members [" + join(view.members) +
        "], coordinator " + view.coordinator() + " (" + std::string(reason) + ")");
    if (shards_ != nullptr) {
        shards_->on_view_changed();
    }
    nudge();
}

void ClusterManager::nudge() {
    {
        const std::scoped_lock lock(wake_mutex_);
        ++view_generation_;
    }
    wake_.notify_all();
}

template <typename Message>
void ClusterManager::heard_from(const std::string& peer, const Message& message) {
    membership_.record_contact(peer, core::Clock::now(), message.incarnation());
    if (shards_ != nullptr) {
        shards_->record_progress(peer, message.has_synced_epoch()
                                           ? std::optional<std::uint64_t>(message.synced_epoch())
                                           : std::nullopt);
    }
    const core::MembershipView theirs = view_from(message);
    if (membership_.observe(theirs)) {
        adopt(theirs, "learned from " + peer);
    }
}

template <typename Message>
void ClusterManager::describe_self(Message* message) const {
    message->set_node_id(membership_.self());
    message->set_incarnation(membership_.incarnation());
    put_view(membership_.view(), message);
    if (shards_ != nullptr) {
        if (const auto synced = shards_->synced_epoch()) {
            message->set_synced_epoch(*synced);
        }
    }
}

void ClusterManager::on_heartbeat(const v1::HeartbeatRequest& request,
                                  v1::HeartbeatResponse* response) {
    heard_from(request.node_id(), request);
    describe_self(response);
}

void ClusterManager::fill_topology(v1::GetTopologyResponse* response) const {
    const core::MembershipView view = membership_.view();
    response->set_epoch(view.epoch);
    response->set_coordinator_node_id(view.coordinator());
    for (const std::string& node : membership_.all_nodes()) {
        v1::NodeStatus* status = response->add_nodes();
        status->set_node_id(node);
        status->set_address(node == membership_.self() ? self_address_ : router_.peers().at(node));
        // "Alive" means "in the agreed view", so every node reports the same.
        status->set_alive(std::ranges::binary_search(view.members, node));
    }
}

core::MembershipView ClusterManager::view() const {
    return membership_.view();
}

void ClusterManager::log(const std::string& message) const {
    if (!config_.log_changes) {
        return;
    }
    const std::scoped_lock lock(log_mutex_);
    std::cout << "[" << membership_.self() << "] " << message << std::endl;
}

}  // namespace matchmesh::node
