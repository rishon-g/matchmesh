#include "matchmesh/node/cluster_service.hpp"

namespace matchmesh::node {

ClusterServiceImpl::ClusterServiceImpl(ClusterManager& manager) : manager_(manager) {}

grpc::Status ClusterServiceImpl::Heartbeat(grpc::ServerContext* /*context*/,
                                           const v1::HeartbeatRequest* request,
                                           v1::HeartbeatResponse* response) {
    manager_.on_heartbeat(*request, response);
    return grpc::Status::OK;
}

grpc::Status ClusterServiceImpl::GetTopology(grpc::ServerContext* /*context*/,
                                             const v1::GetTopologyRequest* /*request*/,
                                             v1::GetTopologyResponse* response) {
    manager_.fill_topology(response);
    return grpc::Status::OK;
}

}  // namespace matchmesh::node
