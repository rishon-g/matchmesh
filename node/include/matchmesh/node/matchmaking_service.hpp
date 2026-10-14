#pragma once

// MatchmakingService: the client-facing API (Enqueue, Cancel, GetLobby).
//
// A client may call any node. Each handler:
//   1. checks the request,
//   2. asks the Router which node owns the queue,
//   3. if it's this node, serves it from the local engine;
//      otherwise forwards it, once, to the owner and relays the answer.
//
// So clients never need to know the cluster layout. All matchmaking logic
// stays in core.
//
// Metadata (gRPC's per-call key/value headers) used here:
//   x-matchmesh-forwarded-by  on a request: set by the node that forwarded
//                             it. A forwarded request is never forwarded
//                             again, which rules out loops.
//   x-matchmesh-epoch         on a forwarded request: the membership epoch
//                             the forwarder routed it under, so a receiver
//                             that disagrees can tell "views still spreading,
//                             retry" (different epochs) from "misconfigured"
//                             (same epoch).
//   x-matchmesh-served-by     on a response: the node that actually ran the
//                             request, so clients and tests can see where it
//                             went.
//
// gRPC runs these handlers on its own thread pool, so several can run at
// once. That's safe because the engine and the router are both thread-safe.

#include <string_view>

#include <grpcpp/grpcpp.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::node {

inline constexpr std::string_view kForwardedByKey = "x-matchmesh-forwarded-by";
inline constexpr std::string_view kServedByKey = "x-matchmesh-served-by";
inline constexpr std::string_view kEpochKey = "x-matchmesh-epoch";

class MatchmakingServiceImpl final : public v1::MatchmakingService::Service {
public:
    // Both are owned by main and outlive the server.
    MatchmakingServiceImpl(core::MatchmakingEngine& engine, const Router& router);

    grpc::Status Enqueue(grpc::ServerContext* context, const v1::EnqueueRequest* request,
                         v1::EnqueueResponse* response) override;

    grpc::Status Cancel(grpc::ServerContext* context, const v1::CancelRequest* request,
                        v1::CancelResponse* response) override;

    grpc::Status GetLobby(grpc::ServerContext* context, const v1::GetLobbyRequest* request,
                          v1::GetLobbyResponse* response) override;

private:
    // Serving a request that this node owns.
    grpc::Status enqueue_locally(const v1::EnqueueRequest& request, v1::EnqueueResponse* response);
    grpc::Status cancel_locally(const v1::CancelRequest& request, v1::CancelResponse* response);
    grpc::Status get_lobby_locally(const v1::GetLobbyRequest& request,
                                   v1::GetLobbyResponse* response);

    // Runs `serve_locally` if this node owns `queue`, otherwise forwards the
    // call with `call_owner`. Shared by all three handlers.
    template <typename Request, typename Response, typename ServeLocally, typename CallOwner>
    grpc::Status route(grpc::ServerContext* context, const core::QueueKey& queue,
                       const Request& request, Response* response, ServeLocally serve_locally,
                       CallOwner call_owner);

    core::MatchmakingEngine& engine_;
    const Router& router_;
};

}  // namespace matchmesh::node
