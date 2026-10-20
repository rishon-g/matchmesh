#include "matchmesh/loadgen/results.hpp"

#include <algorithm>
#include <set>

namespace matchmesh::loadgen {

std::size_t RunResult::count(FinalStatus status) const {
    return static_cast<std::size_t>(std::ranges::count(players, status, &PlayerResult::status));
}

std::size_t RunResult::lobbies() const {
    std::set<std::string> ids;
    for (const PlayerResult& player : players) {
        if (player.status == FinalStatus::kInLobby) {
            ids.insert(player.lobby_id);
        }
    }
    return ids.size();
}

std::size_t RunResult::players_retried() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(players, [](const PlayerResult& p) { return p.attempts > 1; }));
}

}  // namespace matchmesh::loadgen
