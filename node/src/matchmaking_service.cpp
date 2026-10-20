#include "matchmesh/node/matchmaking_service.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {
namespace {

// Returns an INVALID_ARGUMENT status naming the first missing field, or
// nothing if the player ID and queue are filled in.
std::optional<grpc::Status> check_player_and_queue(const std::string& player_id,
                                                   const v1::QueueKey& queue) {
    if (player_id.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "player_id is required");
    }
    if (queue.region().empty() || queue.game_mode().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "queue.region and queue.game_mode are required");
    }
    return std::nullopt;
}

// A request header's value, if the caller sent it.
std::optional<std::string> header(const grpc::ServerContext& context, std::string_view key) {
    const auto& metadata = context.client_metadata();
    const auto it = metadata.find(grpc::string_ref(key.data(), key.size()));
    if (it == metadata.end()) {
        return std::nullopt;
    }
    return std::string(it->second.data(), it->second.size());
}

// The change couldn't be confirmed by the queue's backup (or the queue is
// moving between nodes right now), so it wasn't made. Retrying is safe.
grpc::Status not_replicated(const core::QueueKey& queue) {
    return {grpc::StatusCode::UNAVAILABLE,
            "queue " + core::to_string(queue) +
                " could not be copied to its backup (a node may be down or the queue is "
                "moving); nothing changed, retry"};
}

}  // namespace

MatchmakingServiceImpl::MatchmakingServiceImpl(core::MatchmakingEngine& engine,
                                               const Router& router, ShardManager* shards)
    : engine_(engine), router_(router), shards_(shards) {}

template <typename Request, typename Response, typename ServeLocally, typename CallOwner>
grpc::Status MatchmakingServiceImpl::route(grpc::ServerContext* context,
                                           const core::QueueKey& queue, const Request& request,
                                           Response* response, ServeLocally serve_locally,
                                           CallOwner call_owner) {
    // One routing decision per request, all made under the same view.
    const auto [owner, epoch] = router_.route(queue);

    // 1. Ours: serve it here. served-by also goes in the trailers, which
    //    arrive even with an error, so a forwarder can tell "the owner
    //    answered no" from "the owner couldn't be reached".
    if (owner == router_.self_id()) {
        context->AddInitialMetadata(std::string(kServedByKey), router_.self_id());
        context->AddTrailingMetadata(std::string(kServedByKey), router_.self_id());
        if (shards_ != nullptr) {
            if (auto refused = shards_->admit(queue)) {
                return *refused;  // still moving here; retry shortly
            }
        }
        return serve_locally();
    }

    // 2. Someone else's, but it was already forwarded once, so the forwarder
    //    and this node disagree about the owner. Never forward twice: refuse
    //    rather than bounce it around or split the queue across nodes.
    if (header(*context, kForwardedByKey)) {
        const std::string their_epoch = header(*context, kEpochKey).value_or("");
        if (!their_epoch.empty() && their_epoch != std::to_string(epoch)) {
            // Different epochs: a new membership view is still spreading
            // (this takes milliseconds). Safe to retry shortly.
            return {grpc::StatusCode::UNAVAILABLE,
                    "membership is changing (forwarded under epoch " + their_epoch +
                        ", this node is at epoch " + std::to_string(epoch) + "); retry"};
        }
        // Same epoch yet different owners: the nodes were configured with
        // different peer lists. Retrying won't help.
        return {grpc::StatusCode::FAILED_PRECONDITION,
                router_.self_id() + " does not own queue " + core::to_string(queue) +
                    " (it believes " + owner + " does); check every node's peer list"};
    }

    // 3. Someone else's: forward it to the owner, once.
    grpc::ClientContext outgoing;
    outgoing.AddMetadata(std::string(kForwardedByKey), router_.self_id());
    outgoing.AddMetadata(std::string(kEpochKey), std::to_string(epoch));
    // Never wait longer than the original caller is willing to, and never
    // longer than our own forwarding limit.
    outgoing.set_deadline(std::min(context->deadline(), std::chrono::system_clock::now() +
                                                             router_.forward_timeout()));

    grpc::Status status = call_owner(router_.stub_for(owner), &outgoing, request, response);

    // Which node really served it, if the owner answered at all.
    std::optional<std::string> served_by;
    for (const auto* metadata :
         {&outgoing.GetServerInitialMetadata(), &outgoing.GetServerTrailingMetadata()}) {
        if (const auto it =
                metadata->find(grpc::string_ref(kServedByKey.data(), kServedByKey.size()));
            it != metadata->end()) {
            served_by = std::string(it->second.data(), it->second.size());
            break;
        }
    }

    if (!served_by && (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
                       status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED)) {
        // The owner is down or too slow. Within about 1.5 s it is dropped
        // from the membership and its backup takes over, so: retry.
        return {grpc::StatusCode::UNAVAILABLE, "queue " + core::to_string(queue) +
                                                   " is owned by " + owner +
                                                   ", which is unreachable: " +
                                                   status.error_message()};
    }
    if (served_by) {
        context->AddInitialMetadata(std::string(kServedByKey), *served_by);
        context->AddTrailingMetadata(std::string(kServedByKey), *served_by);
    }
    return status;  // success or the owner's own error, unchanged
}

// ---- Handlers: check, then route ----

grpc::Status MatchmakingServiceImpl::Enqueue(grpc::ServerContext* context,
                                             const v1::EnqueueRequest* request,
                                             v1::EnqueueResponse* response) {
    if (request->player().player_id().empty()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "player.player_id is required"};
    }
    if (request->region().empty() || request->game_mode().empty()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "region and game_mode are required"};
    }
    // The queue comes from region + mode + the bracket of the player's rating.
    const core::QueueKey queue =
        core::make_queue_key(request->region(), request->game_mode(), request->player().rating());

    return route(
        context, queue, *request, response, [&] { return enqueue_locally(*request, response); },
        [](auto& stub, grpc::ClientContext* ctx, const auto& req, auto* resp) {
            return stub.Enqueue(ctx, req, resp);
        });
}

grpc::Status MatchmakingServiceImpl::Cancel(grpc::ServerContext* context,
                                            const v1::CancelRequest* request,
                                            v1::CancelResponse* response) {
    if (auto invalid = check_player_and_queue(request->player_id(), request->queue())) {
        return *invalid;
    }
    return route(
        context, from_proto(request->queue()), *request, response,
        [&] { return cancel_locally(*request, response); },
        [](auto& stub, grpc::ClientContext* ctx, const auto& req, auto* resp) {
            return stub.Cancel(ctx, req, resp);
        });
}

grpc::Status MatchmakingServiceImpl::GetLobby(grpc::ServerContext* context,
                                              const v1::GetLobbyRequest* request,
                                              v1::GetLobbyResponse* response) {
    if (auto invalid = check_player_and_queue(request->player_id(), request->queue())) {
        return *invalid;
    }
    return route(
        context, from_proto(request->queue()), *request, response,
        [&] { return get_lobby_locally(*request, response); },
        [](auto& stub, grpc::ClientContext* ctx, const auto& req, auto* resp) {
            return stub.GetLobby(ctx, req, resp);
        });
}

// ---- Local serving (unchanged logic from before) ----

grpc::Status MatchmakingServiceImpl::enqueue_locally(const v1::EnqueueRequest& request,
                                                     v1::EnqueueResponse* response) {
    const core::EnqueueOutcome outcome =
        engine_.enqueue(core::Player{request.player().player_id(), request.player().rating()},
                        request.region(), request.game_mode(), request.request_id());

    switch (outcome.result) {
        case core::EnqueueResult::kAdded:
        case core::EnqueueResult::kAlreadyDone:  // a retry of a join that worked: same answer
            to_proto(outcome.queue, response->mutable_queue());
            to_proto(outcome.enqueued_at, response->mutable_enqueued_at());
            return grpc::Status::OK;
        case core::EnqueueResult::kDuplicate:
            return {grpc::StatusCode::ALREADY_EXISTS, "player is already in this queue"};
        case core::EnqueueResult::kInvalidPlayer:
            return {grpc::StatusCode::INVALID_ARGUMENT, "invalid player"};
        case core::EnqueueResult::kNotPrimary:
        case core::EnqueueResult::kNotReplicated:
            return not_replicated(outcome.queue);
        case core::EnqueueResult::kWrongBracket:
            // The engine picks the bracket from the rating, so this means a bug.
            break;
    }
    return {grpc::StatusCode::INTERNAL, "unexpected enqueue result"};
}

grpc::Status MatchmakingServiceImpl::cancel_locally(const v1::CancelRequest& request,
                                                    v1::CancelResponse* response) {
    switch (engine_.cancel(from_proto(request.queue()), request.player_id())) {
        case core::CancelResult::kCancelled:
            response->set_cancelled(true);
            return grpc::Status::OK;
        case core::CancelResult::kNotWaiting:
            response->set_cancelled(false);
            return grpc::Status::OK;
        case core::CancelResult::kNotPrimary:
        case core::CancelResult::kNotReplicated:
            break;
    }
    return not_replicated(from_proto(request.queue()));
}

grpc::Status MatchmakingServiceImpl::get_lobby_locally(const v1::GetLobbyRequest& request,
                                                       v1::GetLobbyResponse* response) {
    const core::PlayerLookup found = engine_.lookup(from_proto(request.queue()), request.player_id());

    switch (found.status) {
        case core::PlayerStatus::kQueued:
            response->set_status(v1::PLAYER_STATUS_QUEUED);
            break;
        case core::PlayerStatus::kInLobby:
            response->set_status(v1::PLAYER_STATUS_IN_LOBBY);
            to_proto(*found.lobby, response->mutable_lobby());
            break;
        case core::PlayerStatus::kNotFound:
            // Not an error: "not in this queue" is a normal answer.
            response->set_status(v1::PLAYER_STATUS_NOT_FOUND);
            break;
    }
    return grpc::Status::OK;
}

}  // namespace matchmesh::node
