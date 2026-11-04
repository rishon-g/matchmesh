#include "matchmesh/node/replication_client.hpp"

#include <utility>

#include <grpcpp/grpcpp.h>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {

ReplicationClient::ReplicationClient(const Router& router, std::chrono::milliseconds timeout)
    : router_(router), timeout_(timeout), backups_(router, *this) {
    for (const auto& [id, address] : router_.peers()) {
        stubs_.emplace(id, v1::ClusterService::NewStub(make_peer_channel(address)));
    }
}

bool ReplicationClient::replicate(const core::ShardChange& change,
                                  const std::function<core::ShardState()>& current) {
    return backups_.replicate(change, current);
}

bool ReplicationClient::sync(const core::QueueKey& queue,
                             const std::function<core::ShardState()>& current) {
    return backups_.sync(queue, current);
}

bool ReplicationClient::hand_over(const std::string& target, std::uint64_t epoch,
                                  const core::ShardState& state) {
    return send_snapshot(target, epoch, state);
}

bool ReplicationClient::send_change(const std::string& target, std::uint64_t epoch,
                                    const core::ShardChange& change) {
    v1::ReplicateRequest request;
    to_proto(change, &request);
    return send(target, epoch, request);
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

}  // namespace matchmesh::node
