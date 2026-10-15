#pragma once

// ReplicationClient: the network half of primary-backup replication, as seen
// from the sending side. The engine calls it (through core::Replicator) for
// every change to a queue this node owns; it sends the change to the queue's
// backup with ClusterService.Replicate and reports whether the backup
// applied it.
//
// It also remembers, per queue, whether the backup is known to hold a
// complete copy. If not (a new backup, a new epoch, or a change the backup
// just rejected), it sends a full snapshot first. That one rule covers a
// backup that restarted, missed a change, or was only just assigned.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "matchmesh/core/replication.hpp"
#include "matchmesh/core/types.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"

namespace matchmesh::node {

class ReplicationClient final : public core::Replicator {
public:
    // `timeout` bounds each Replicate call: a backup that doesn't answer in
    // time counts as having refused, so the player gets a retryable error
    // instead of waiting. The router must outlive this.
    explicit ReplicationClient(const Router& router,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

    // core::Replicator. Both refuse (return false) if, under the router's
    // current view, this node isn't the queue's primary any more.
    bool replicate(const core::ShardChange& change,
                   const std::function<core::ShardState()>& current) override;
    bool sync(const core::QueueKey& queue,
              const std::function<core::ShardState()>& current) override;

    // Sends a complete copy of a queue to `target`, its new owner, as part of
    // the hand-over after a membership change. True once it was accepted.
    bool hand_over(const std::string& target, std::uint64_t epoch, const core::ShardState& state);

private:
    // Sends `request` (after filling in sender and epoch). True if accepted.
    bool send(const std::string& target, std::uint64_t epoch, v1::ReplicateRequest& request);
    bool send_snapshot(const std::string& target, std::uint64_t epoch,
                       const core::ShardState& state);

    // Whether `backup` is known to hold a complete copy of `queue` as of
    // `epoch`, and recording that it does (or forgetting it).
    bool is_synced(const core::QueueKey& queue, const std::string& backup, std::uint64_t epoch);
    void mark_synced(const core::QueueKey& queue, const std::string& backup, std::uint64_t epoch);
    void forget(const core::QueueKey& queue);

    struct SyncedCopy {
        std::string backup;
        std::uint64_t epoch = 0;
    };

    const Router& router_;
    const std::chrono::milliseconds timeout_;
    std::unordered_map<std::string, std::unique_ptr<v1::ClusterService::Stub>> stubs_;

    std::mutex mutex_;  // guards synced_
    std::unordered_map<core::QueueKey, SyncedCopy, core::QueueKeyHash> synced_;
};

}  // namespace matchmesh::node
