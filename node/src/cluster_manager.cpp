#include "matchmesh/node/cluster_manager.hpp"

#include <algorithm>
#include <iostream>
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

core::MembershipView view_from(std::uint64_t epoch,
                               const google::protobuf::RepeatedPtrField<std::string>& members) {
    return core::MembershipView{epoch, {members.begin(), members.end()}};
}

template <typename Message>
void put_view(const core::MembershipView& view, Message* message) {
    message->set_epoch(view.epoch);
    for (const std::string& member : view.members) {
        message->add_members(member);
    }
}

}  // namespace

ClusterManager::ClusterManager(core::Membership& membership, Router& router,
                               std::string self_address, ClusterManagerConfig config)
    : membership_(membership),
      router_(router),
      self_address_(std::move(self_address)),
      config_(config) {
    for (const auto& [id, address] : router_.peers()) {
        stubs_.emplace(id, v1::ClusterService::NewStub(make_peer_channel(address)));
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
        request.set_node_id(membership_.self());
        put_view(membership_.view(), &request);

        grpc::ClientContext context;
        // Fail fast if the peer is down; don't wait for it to come up.
        context.set_deadline(std::chrono::system_clock::now() + config_.heartbeat_timeout);
        v1::HeartbeatResponse response;
        if (stub.Heartbeat(&context, request, &response).ok()) {
            membership_.record_contact(peer, core::Clock::now());
            const auto theirs = view_from(response.epoch(), response.members());
            if (membership_.observe(theirs)) {
                adopt(theirs, "learned from " + peer);
            }
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
    {
        const std::scoped_lock lock(wake_mutex_);
        ++view_generation_;
    }
    wake_.notify_all();
}

void ClusterManager::on_heartbeat(const v1::HeartbeatRequest& request,
                                  v1::HeartbeatResponse* response) {
    membership_.record_contact(request.node_id(), core::Clock::now());
    const auto theirs = view_from(request.epoch(), request.members());
    if (membership_.observe(theirs)) {
        adopt(theirs, "learned from " + request.node_id());
    }
    response->set_node_id(membership_.self());
    put_view(membership_.view(), response);
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
