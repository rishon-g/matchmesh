#pragma once

// ClusterService: node-to-node calls.
//   Heartbeat    failure detection, and how membership views spread
//   GetTopology  the current view and each node's status
// Both hand off to the ClusterManager. Replicate returns UNIMPLEMENTED until
// replication arrives in the next commit.

#include <grpcpp/grpcpp.h>

#include "matchmesh/node/cluster_manager.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"

namespace matchmesh::node {

class ClusterServiceImpl final : public v1::ClusterService::Service {
public:
    explicit ClusterServiceImpl(ClusterManager& manager);

    grpc::Status Heartbeat(grpc::ServerContext* context, const v1::HeartbeatRequest* request,
                           v1::HeartbeatResponse* response) override;

    grpc::Status GetTopology(grpc::ServerContext* context, const v1::GetTopologyRequest* request,
                             v1::GetTopologyResponse* response) override;

private:
    ClusterManager& manager_;
};

}  // namespace matchmesh::node
