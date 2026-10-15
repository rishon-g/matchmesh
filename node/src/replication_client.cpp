#include "matchmesh/node/replication_client.hpp"

#include <utility>

#include <grpcpp/grpcpp.h>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {

ReplicationClient::ReplicationClient(const Router& router, std::chrono::milliseconds timeout)
    : router_(router), timeout_(timeout) {
    for (const auto& [id, address] : router_.peers()) {
        stubs_.emplace(id, v1::ClusterService::NewStub(make_peer_channel(address)));
    }
}

bool ReplicationClient::replicate(const core::ShardChange& change,
                                  const std::function<core::ShardState()>& current) {
    const Placement place = router_.placement(change.queue);
    if (place.primary != router_.self_id()) {
        return false;  // the queue has moved on; its new owner takes changes now
    }
    if (!place.backup) {
        return true;  // a one-node cluster: nobody to copy to
    }
    const std::string& backup = *place.backup;

    // A backup that may not hold a complete copy gets one first, as of just
    // before this change, so the change itself then fits.
    if (!is_synced(change.queue, backup, place.epoch)) {
        if (!send_snapshot(backup, place.epoch, current())) {
            return false;
        }
        mark_synced(change.queue, backup, place.epoch);
    }

    v1::ReplicateRequest request;
    to_proto(change, &request);
    if (!send(backup, place.epoch, request)) {
        // We can't know whether the backup applied it (the reply may be what
        // got lost), so don't trust its copy: the next change sends a fresh
        // snapshot, which puts both copies back in step.
        forget(change.queue);
        return false;
    }
    return true;
}

bool ReplicationClient::sync(const core::QueueKey& queue,
                             const std::function<core::ShardState()>& current) {
    const Placement place = router_.placement(queue);
    if (place.primary != router_.self_id()) {
        return false;
    }
    if (!place.backup) {
        return true;
    }
    if (is_synced(queue, *place.backup, place.epoch)) {
        return true;
    }
    if (!send_snapshot(*place.backup, place.epoch, current())) {
        return false;
    }
    mark_synced(queue, *place.backup, place.epoch);
    return true;
}

bool ReplicationClient::hand_over(const std::string& target, std::uint64_t epoch,
                                  const core::ShardState& state) {
    return send_snapshot(target, epoch, state);
}

bool ReplicationClient::send_snapshot(const std::string& target, std::uint64_t epoch,
                                      const core::ShardState& state) {
    v1::ReplicateRequest request;
    to_proto(state, &request);
    return send(target, epoch, request);
}

bool ReplicationClient::send(const std::string& target, std::uint64_t epoch,
                             v1::ReplicateRequest& request) {
    const auto stub = stubs_.find(target);
    if (stub == stubs_.end()) {
        return false;  // not a node we have an address for
    }
    request.set_sender_node_id(router_.self_id());
    request.set_epoch(epoch);

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + timeout_);
    v1::ReplicateResponse response;
    return stub->second->Replicate(&context, request, &response).ok() && response.accepted();
}

bool ReplicationClient::is_synced(const core::QueueKey& queue, const std::string& backup,
                                  std::uint64_t epoch) {
    const std::scoped_lock lock(mutex_);
    const auto it = synced_.find(queue);
    // Any epoch change re-sends the copy: the backup may have restarted or
    // dropped the queue in between, and epochs are rare.
    return it != synced_.end() && it->second.backup == backup && it->second.epoch == epoch;
}

void ReplicationClient::mark_synced(const core::QueueKey& queue, const std::string& backup,
                                    std::uint64_t epoch) {
    const std::scoped_lock lock(mutex_);
    synced_[queue] = SyncedCopy{backup, epoch};
}

void ReplicationClient::forget(const core::QueueKey& queue) {
    const std::scoped_lock lock(mutex_);
    synced_.erase(queue);
}

}  // namespace matchmesh::node
