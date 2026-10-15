#pragma once

// ClusterService: node-to-node calls.
//   Heartbeat    failure detection, and how membership views spread
//   Replicate    primary -> backup changes, and hand-overs (ShardManager)
//   GetTopology  the current view and each node's status
// Heartbeat and GetTopology go to the ClusterManager; Replicate to the
// ShardManager (UNIMPLEMENTED without one).

#include <grpcpp/grpcpp.h>

#include "matchmesh/node/cluster_manager.hpp"
#include "matchmesh/node/shard_manager.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"

namespace matchmesh::node {

class ClusterServiceImpl final : public v1::ClusterService::Service {
public:
    explicit ClusterServiceImpl(ClusterManager& manager, ShardManager* shards = nullptr);

    grpc::Status Heartbeat(grpc::ServerContext* context, const v1::HeartbeatRequest* request,
                           v1::HeartbeatResponse* response) override;

    grpc::Status Replicate(grpc::ServerContext* context, const v1::ReplicateRequest* request,
                           v1::ReplicateResponse* response) override;

    grpc::Status GetTopology(grpc::ServerContext* context, const v1::GetTopologyRequest* request,
                             v1::GetTopologyResponse* response) override;

private:
    ClusterManager& manager_;
    ShardManager* const shards_;
};

}  // namespace matchmesh::node
