#include "matchmesh/node/convert.hpp"

#include <chrono>
#include <cstdint>

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

void to_proto(core::TimePoint when, google::protobuf::Timestamp* out) {
    using namespace std::chrono;
    const auto age = duration_cast<system_clock::duration>(core::Clock::now() - when);
    const std::int64_t nanos_since_epoch =
        duration_cast<nanoseconds>((system_clock::now() - age).time_since_epoch()).count();

    constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
    out->set_seconds(nanos_since_epoch / kNanosPerSecond);
    out->set_nanos(static_cast<std::int32_t>(nanos_since_epoch % kNanosPerSecond));
}

}  // namespace matchmesh::node
