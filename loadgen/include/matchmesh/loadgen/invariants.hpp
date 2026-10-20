#pragma once

// The lost-player invariant checker: after a load test (with or without
// nodes being killed during it), checks that the cluster's final answers add
// up. Every player who was told "you're in the queue" must now be in exactly
// one place:
//
//   * in exactly one lobby, and that lobby agrees: everyone who reports it
//     lists the same members, it has lobby_size players, all from the same
//     queue, and every member reports being in it; or
//   * still queued, and in no lobby.
//
// Anything else is a violation:
//   lost            enqueued, but the cluster says it has never heard of them
//                   (or can't say where they are)
//   in two places   in two different lobbies, or in a lobby and still queued
//   bad lobby       wrong size, members disagree about who's in it, players
//                   from different queues, or a member we never sent
//   ghost           a player whose enqueue was reported as failed, found in
//                   a lobby anyway (their client would retry and duplicate them)
//
// Players whose enqueue failed (and who aren't ghosts) aren't violations:
// they were told so. The load generator reports them separately.
//
// Pure logic over the results; no networking.

#include <cstddef>
#include <string>
#include <vector>

#include "matchmesh/loadgen/results.hpp"

namespace matchmesh::loadgen {

struct InvariantReport {
    std::size_t players = 0;
    std::size_t in_one_lobby = 0;
    std::size_t queued = 0;
    std::size_t never_enqueued = 0;

    std::size_t lost = 0;
    std::size_t in_two_places = 0;
    std::size_t bad_lobbies = 0;
    std::size_t ghosts = 0;

    // A readable line per problem found (the first `kMaxProblems`).
    std::vector<std::string> problems;
    std::size_t problems_not_shown = 0;
    static constexpr std::size_t kMaxProblems = 20;

    [[nodiscard]] bool ok() const noexcept {
        return lost == 0 && in_two_places == 0 && bad_lobbies == 0 && ghosts == 0;
    }
};

[[nodiscard]] InvariantReport check_invariants(const std::vector<PlayerResult>& players,
                                               std::size_t lobby_size = 4);

}  // namespace matchmesh::loadgen
