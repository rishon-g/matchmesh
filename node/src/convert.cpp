#include "matchmesh/node/convert.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace matchmesh::node {

void to_proto(const core::Player& in, v1::Player* out) {
    out->set_player_id(in.id);
    out->set_rating(in.rating);
}

void to_proto(const core::QueueKey& in, v1::QueueKey* out) {
    out->set_region(in.region);
    out->set_game_mode(in.game_mode);
    out->set_skill_bracket(in.skill_bracket);
}

void to_proto(const core::Lobby& in, v1::Lobby* out) {
    out->set_lobby_id(in.lobby_id);
    to_proto(in.queue, out->mutable_queue());
    for (const core::Player& player : in.players) {
        to_proto(player, out->add_players());
    }
    to_proto(in.formed_at, out->mutable_formed_at());
}

core::QueueKey from_proto(const v1::QueueKey& in) {
    return core::QueueKey{in.region(), in.game_mode(), in.skill_bracket()};
}

core::Player from_proto(const v1::Player& in) {
    return core::Player{in.player_id(), in.rating()};
}

core::Lobby from_proto(const v1::Lobby& in) {
    core::Lobby out{in.lobby_id(), from_proto(in.queue()), {}, from_proto(in.formed_at())};
    out.players.reserve(static_cast<std::size_t>(in.players_size()));
    for (const v1::Player& player : in.players()) {
        out.players.push_back(from_proto(player));
    }
    return out;
}

void to_proto(core::TimePoint when, google::protobuf::Timestamp* out) {
    using namespace std::chrono;
    const auto age = duration_cast<system_clock::duration>(core::Clock::now() - when);
    const std::int64_t nanos_since_epoch =
        duration_cast<nanoseconds>((system_clock::now() - age).time_since_epoch()).count();

    constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
    out->set_seconds(nanos_since_epoch / kNanosPerSecond);
    out->set_nanos(static_cast<std::int32_t>(nanos_since_epoch % kNanosPerSecond));
}

core::TimePoint from_proto(const google::protobuf::Timestamp& in) {
    using namespace std::chrono;
    const system_clock::time_point wall{duration_cast<system_clock::duration>(
        seconds(in.seconds()) + nanoseconds(in.nanos()))};
    const auto age = system_clock::now() - wall;
    return core::Clock::now() - duration_cast<core::Clock::duration>(age);
}

void to_proto(const core::ShardChange& in, v1::ReplicateRequest* out) {
    to_proto(in.queue, out->mutable_queue());
    out->set_sequence(in.sequence);
    if (const auto* joined = std::get_if<core::PlayerJoined>(&in.op)) {
        to_proto(joined->player, out->mutable_enqueue()->mutable_player());
        to_proto(joined->enqueued_at, out->mutable_enqueue()->mutable_enqueued_at());
        out->mutable_enqueue()->set_request_id(joined->request_id);
    } else if (const auto* left = std::get_if<core::PlayerLeft>(&in.op)) {
        out->mutable_cancel()->set_player_id(left->player_id);
    } else {
        to_proto(std::get<core::LobbyFormed>(in.op).lobby,
                 out->mutable_lobby_formed()->mutable_lobby());
    }
}

void to_proto(const core::ShardState& in, v1::ReplicateRequest* out) {
    to_proto(in.queue, out->mutable_queue());
    out->set_sequence(in.sequence);
    v1::ShardSnapshot* snapshot = out->mutable_snapshot();
    for (const core::PlayerJoined& waiting : in.waiting) {
        v1::WaitingPlayer* entry = snapshot->add_waiting();
        to_proto(waiting.player, entry->mutable_player());
        to_proto(waiting.enqueued_at, entry->mutable_enqueued_at());
        entry->set_request_id(waiting.request_id);
    }
    for (const core::Lobby& lobby : in.lobbies) {
        to_proto(lobby, snapshot->add_lobbies());
    }
    for (const auto& [player_id, lobby_id] : in.lobby_of) {
        (*snapshot->mutable_lobby_of())[player_id] = lobby_id;
    }
    for (const auto& [player_id, ticket] : in.tickets) {
        v1::JoinTicket& out_ticket = (*snapshot->mutable_tickets())[player_id];
        out_ticket.set_request_id(ticket.request_id);
        to_proto(ticket.enqueued_at, out_ticket.mutable_enqueued_at());
    }
}

std::optional<core::ShardChange> change_from_proto(const v1::ReplicateRequest& in) {
    core::ShardChange change{from_proto(in.queue()), in.sequence(), core::PlayerLeft{}};
    switch (in.op_case()) {
        case v1::ReplicateRequest::kEnqueue:
            change.op = core::PlayerJoined{from_proto(in.enqueue().player()),
                                           from_proto(in.enqueue().enqueued_at()),
                                           in.enqueue().request_id()};
            return change;
        case v1::ReplicateRequest::kCancel:
            change.op = core::PlayerLeft{in.cancel().player_id()};
            return change;
        case v1::ReplicateRequest::kLobbyFormed:
            change.op = core::LobbyFormed{from_proto(in.lobby_formed().lobby())};
            return change;
        case v1::ReplicateRequest::kSnapshot:
        case v1::ReplicateRequest::OP_NOT_SET:
            break;
    }
    return std::nullopt;
}

core::ShardState state_from_proto(const v1::ReplicateRequest& in) {
    core::ShardState state{from_proto(in.queue()), in.sequence(), {}, {}, {}, {}};
    const v1::ShardSnapshot& snapshot = in.snapshot();
    state.waiting.reserve(static_cast<std::size_t>(snapshot.waiting_size()));
    for (const v1::WaitingPlayer& waiting : snapshot.waiting()) {
        state.waiting.push_back(core::PlayerJoined{from_proto(waiting.player()),
                                                   from_proto(waiting.enqueued_at()),
                                                   waiting.request_id()});
    }
    for (const v1::Lobby& lobby : snapshot.lobbies()) {
        state.lobbies.push_back(from_proto(lobby));
    }
    state.lobby_of.insert(snapshot.lobby_of().begin(), snapshot.lobby_of().end());
    for (const auto& [player_id, ticket] : snapshot.tickets()) {
        state.tickets.emplace(player_id,
                              core::JoinTicket{ticket.request_id(), from_proto(ticket.enqueued_at())});
    }
    return state;
}

}  // namespace matchmesh::node
