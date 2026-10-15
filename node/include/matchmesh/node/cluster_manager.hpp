#pragma once

// ClusterManager: the network side of membership.
//
//   * One heartbeat thread per peer sends a Heartbeat every 500 ms, carrying
//     this node's current view. A reply counts as contact with that peer,
//     and the peer's view in the reply is adopted if it's newer.
//   * Incoming heartbeats (on_heartbeat) count as contact too, and their
//     views are adopted the same way.
//   * A check thread runs every 100 ms: it logs nodes going quiet or coming
//     back, and lets core::Membership publish a new epoch if this node is
//     the coordinator and the alive set changed.
//   * Whenever this node's view changes, the Router switches to it and every
//     heartbeat thread sends straight away, so a new view reaches the whole
//     cluster in milliseconds instead of waiting for the next beat.
//
//   * With a ShardManager, heartbeats also carry how far this node has got
//     handing queues over after a view change, and pass on what peers
//     report, so a queue's new owner knows when it may start serving. When
//     that progress changes, heartbeats go out at once, like a view change.
//
// Timing with the defaults: a dead node is suspected after 1.5 s of silence,
// the coordinator notices within the next 100 ms check, and the new view
// spreads immediately, which keeps recovery under 2 seconds.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "matchmesh/core/membership.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/node/shard_manager.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"

namespace matchmesh::node {

struct ClusterManagerConfig {
    std::chrono::milliseconds heartbeat_interval{500};
    std::chrono::milliseconds heartbeat_timeout{400};  // per heartbeat call
    std::chrono::milliseconds check_interval{100};
    bool log_changes = true;  // print membership changes to stdout
};

class ClusterManager {
public:
    // `self_address` is only used to report this node in GetTopology. The
    // membership, router and shard manager (optional) are owned by the
    // caller and must outlive this.
    ClusterManager(core::Membership& membership, Router& router, std::string self_address,
                   ClusterManagerConfig config = {}, ShardManager* shards = nullptr);

    // Stops the threads (same as stop()).
    ~ClusterManager();

    ClusterManager(const ClusterManager&) = delete;
    ClusterManager& operator=(const ClusterManager&) = delete;

    // Starts the heartbeat and check threads. Call once the server is up.
    void start();
    // Stops and joins the threads. Safe to call more than once.
    void stop();

    // ClusterService.Heartbeat: record contact, adopt a newer view, reply
    // with ours.
    void on_heartbeat(const v1::HeartbeatRequest& request, v1::HeartbeatResponse* response);

    // ClusterService.GetTopology: the current view and each node's status.
    void fill_topology(v1::GetTopologyResponse* response) const;

    [[nodiscard]] core::MembershipView view() const;

private:
    void heartbeat_loop(std::stop_token stop, const std::string& peer);
    void check_loop(std::stop_token stop);

    // Called whenever this node's view may have changed: switch the router,
    // log it and wake the heartbeat threads to spread it.
    void adopt(const core::MembershipView& view, std::string_view reason);
    // Wakes the heartbeat threads so they send now.
    void nudge();
    // What a peer told us about itself in a heartbeat (either direction).
    template <typename Message>
    void heard_from(const std::string& peer, const Message& message);
    // Fills in a heartbeat's description of this node.
    template <typename Message>
    void describe_self(Message* message) const;
    void log(const std::string& message) const;

    core::Membership& membership_;
    Router& router_;
    const std::string self_address_;
    const ClusterManagerConfig config_;
    ShardManager* const shards_;  // may be null
    std::unordered_map<std::string, std::unique_ptr<v1::ClusterService::Stub>> stubs_;

    // Wakes the heartbeat threads early when the view changes.
    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
    std::uint64_t view_generation_ = 0;  // guarded by wake_mutex_

    mutable std::mutex log_mutex_;  // keeps log lines from interleaving

    // Declared last so it is destroyed first.
    std::vector<std::jthread> threads_;
};

}  // namespace matchmesh::node
