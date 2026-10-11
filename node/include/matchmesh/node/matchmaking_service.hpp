#pragma once

// MatchmakingService: the client-facing API (Enqueue, Cancel, GetLobby).
//
// A thin layer: check the request, translate it into core types, call the
// engine, translate the answer back. All matchmaking logic stays in core.
//
// gRPC runs these handlers on its own thread pool, so several can run at
// once. That's safe because the engine is thread-safe.

#include <grpcpp/grpcpp.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::node {

class MatchmakingServiceImpl final : public v1::MatchmakingService::Service {
public:
    explicit MatchmakingServiceImpl(core::MatchmakingEngine& engine);

    grpc::Status Enqueue(grpc::ServerContext* context, const v1::EnqueueRequest* request,
                         v1::EnqueueResponse* response) override;

    grpc::Status Cancel(grpc::ServerContext* context, const v1::CancelRequest* request,
                        v1::CancelResponse* response) override;

    grpc::Status GetLobby(grpc::ServerContext* context, const v1::GetLobbyRequest* request,
                          v1::GetLobbyResponse* response) override;

private:
    core::MatchmakingEngine& engine_;  // owned by main; outlives the server
};

}  // namespace matchmesh::node
