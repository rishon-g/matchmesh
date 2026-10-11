#pragma once

// ClusterService: node-to-node calls. Only Heartbeat is implemented so far;
// Replicate and GetTopology return UNIMPLEMENTED until Phases 5 and 7.

#include <string>

#include <grpcpp/grpcpp.h>

#include "matchmesh/v1/cluster.grpc.pb.h"

namespace matchmesh::node {

class ClusterServiceImpl final : public v1::ClusterService::Service {
public:
    explicit ClusterServiceImpl(std::string node_id);

    grpc::Status Heartbeat(grpc::ServerContext* context, const v1::HeartbeatRequest* request,
                           v1::HeartbeatResponse* response) override;

private:
    std::string node_id_;
};

}  // namespace matchmesh::node
