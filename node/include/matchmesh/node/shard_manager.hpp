#pragma once

// ShardManager: keeps each queue's data on the nodes the ring says should
// hold it, as membership changes. Primary-backup replication itself happens
// in the engine (through ReplicationClient); this class handles everything
// around it:
//
//   * Receiving. ClusterService.Replicate lands here: a change from a
//     queue's primary is applied to this node's replica, strictly in order;
//     a snapshot replaces the replica, or hands a queue to this node as its
//     new owner. Anything sent under a different epoch, or by a node that
//     isn't the queue's primary, is rejected, so a stale node can never
//     overwrite newer state.
//
//   * Handing over. After every view change a background thread walks every
//     queue this node holds. One it no longer owns is demoted (so it stops
//     changing here) and a complete copy is sent to its new owner; then it is
//     dropped, unless this node is now its backup. One it still owns gets its
//     (possibly new) backup brought up to date.
//
//   * Taking over. A queue that just became this node's (a backup whose
//     primary died, or a queue handed back to a node that rejoined) is served
//     only once the hand-over is finished everywhere: every member of the
//     view has reported, through heartbeats, that it has handed over all it
//     holds for the current epoch. Until then requests for it get a
//     retryable UNAVAILABLE. The wait is a few milliseconds, and it means the
//     new owner starts from the newest copy anyone had, never from nothing.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/core/types.hpp"
#include "matchmesh/node/replication_client.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/cluster.pb.h"

namespace matchmesh::node {

struct ShardManagerConfig {
    // How often unfinished work (a hand-over to a node that didn't answer, a
    // backup that needs a copy) is retried, if no view change comes first.
    std::chrono::milliseconds retry_interval{100};
    bool log_changes = true;  // print hand-overs and take-overs to stdout
};

class ShardManager {
public:
    // `incarnation` identifies this process (see core::Membership). Engine,
    // router and replication client must outlive this.
    ShardManager(core::MatchmakingEngine& engine, const Router& router,
                 ReplicationClient& replication, std::uint64_t incarnation,
                 ShardManagerConfig config = {});
    ~ShardManager();  // stop()

    ShardManager(const ShardManager&) = delete;
    ShardManager& operator=(const ShardManager&) = delete;

    // Called (from the background thread) whenever synced_epoch() changes,
    // so heartbeats can report it straight away. Set before start().
    void set_on_synced(std::function<void()> callback);

    void start();
    void stop();  // safe to call more than once

    // The router switched to a new view: start handing over.
    void on_view_changed();

    // A peer's heartbeat reported the latest epoch it has finished handing
    // over for (nothing if it hasn't finished any yet).
    void record_progress(const std::string& node, std::optional<std::uint64_t> synced_epoch);
    // Ours, to put in heartbeats.
    [[nodiscard]] std::optional<std::uint64_t> synced_epoch() const;

    // ClusterService.Replicate.
    void handle(const v1::ReplicateRequest& request, v1::ReplicateResponse* response);

    // The router says this node owns `queue`. Nothing if a client request
    // for it may be served here right now; otherwise the retryable status to
    // answer with. Promotes a waiting copy once the hand-over is finished.
    [[nodiscard]] std::optional<grpc::Status> admit(const core::QueueKey& queue);

    // True when `view` lists this exact process and every member has
    // reported finishing its hand-over for view.epoch.
    [[nodiscard]] bool settled(const core::MembershipView& view) const;

private:
    void loop(std::stop_token stop);
    // Hands every queue this node no longer owns to its owner, counting the
    // copies sent in `sent`. True if all of them were accepted and the view
    // is still `view`; otherwise the next pass tries again.
    bool hand_over_all(const core::MembershipView& view, std::size_t& sent);
    // Once settled, starts serving every copy this node now owns.
    void promote_ready(const core::MembershipView& view);
    // Makes sure every queue this node owns has an up-to-date backup.
    void sync_backups();
    void log(const std::string& message) const;

    core::MatchmakingEngine& engine_;
    const Router& router_;
    ReplicationClient& replication_;
    const std::uint64_t incarnation_;
    const ShardManagerConfig config_;
    std::function<void()> on_synced_;

    mutable std::mutex mutex_;  // guards progress_ and synced_epoch_
    std::map<std::string, std::uint64_t> progress_;  // peer -> its synced epoch
    std::optional<std::uint64_t> synced_epoch_;

    // Wakes the background thread early on a view change.
    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
    std::uint64_t generation_ = 0;  // guarded by wake_mutex_

    mutable std::mutex log_mutex_;

    std::jthread thread_;  // last, so it stops before anything it uses goes away
};

}  // namespace matchmesh::node
