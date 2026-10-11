#include "matchmesh/node/cluster_service.hpp"

#include <iostream>
#include <utility>

namespace matchmesh::node {

ClusterServiceImpl::ClusterServiceImpl(std::string node_id) : node_id_(std::move(node_id)) {}

grpc::Status ClusterServiceImpl::Heartbeat(grpc::ServerContext* context,
                                           const v1::HeartbeatRequest* request,
                                           v1::HeartbeatResponse* response) {
    std::cout << "heartbeat from " << request->node_id() << " (" << context->peer() << ")"
              << std::endl;
    response->set_node_id(node_id_);
    response->set_epoch(0);  // membership epochs arrive in Phase 5
    return grpc::Status::OK;
}

}  // namespace matchmesh::node
