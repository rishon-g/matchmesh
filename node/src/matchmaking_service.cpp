#include "matchmesh/node/matchmaking_service.hpp"

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

}  // namespace

MatchmakingServiceImpl::MatchmakingServiceImpl(core::MatchmakingEngine& engine) : engine_(engine) {}

grpc::Status MatchmakingServiceImpl::Enqueue(grpc::ServerContext* /*context*/,
                                             const v1::EnqueueRequest* request,
                                             v1::EnqueueResponse* response) {
    if (request->player().player_id().empty()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "player.player_id is required"};
    }
    if (request->region().empty() || request->game_mode().empty()) {
        return {grpc::StatusCode::INVALID_ARGUMENT, "region and game_mode are required"};
    }

    const core::EnqueueOutcome outcome =
        engine_.enqueue(core::Player{request->player().player_id(), request->player().rating()},
                        request->region(), request->game_mode());

    switch (outcome.result) {
        case core::EnqueueResult::kAdded:
            to_proto(outcome.queue, response->mutable_queue());
            to_proto(outcome.enqueued_at, response->mutable_enqueued_at());
            return grpc::Status::OK;
        case core::EnqueueResult::kDuplicate:
            return {grpc::StatusCode::ALREADY_EXISTS, "player is already in this queue"};
        case core::EnqueueResult::kInvalidPlayer:
            return {grpc::StatusCode::INVALID_ARGUMENT, "invalid player"};
        case core::EnqueueResult::kWrongBracket:
            // The engine picks the bracket from the rating, so this means a bug.
            break;
    }
    return {grpc::StatusCode::INTERNAL, "unexpected enqueue result"};
}

grpc::Status MatchmakingServiceImpl::Cancel(grpc::ServerContext* /*context*/,
                                            const v1::CancelRequest* request,
                                            v1::CancelResponse* response) {
    if (auto invalid = check_player_and_queue(request->player_id(), request->queue())) {
        return *invalid;
    }
    response->set_cancelled(engine_.cancel(from_proto(request->queue()), request->player_id()));
    return grpc::Status::OK;
}

grpc::Status MatchmakingServiceImpl::GetLobby(grpc::ServerContext* /*context*/,
                                              const v1::GetLobbyRequest* request,
                                              v1::GetLobbyResponse* response) {
    if (auto invalid = check_player_and_queue(request->player_id(), request->queue())) {
        return *invalid;
    }

    const core::PlayerLookup found =
        engine_.lookup(from_proto(request->queue()), request->player_id());

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
