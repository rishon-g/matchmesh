#include "matchmesh/loadgen/invariants.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "matchmesh/core/types.hpp"

namespace matchmesh::loadgen {
namespace {

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

std::vector<std::string> sorted(std::vector<std::string> names) {
    std::ranges::sort(names);
    return names;
}

}  // namespace

InvariantReport check_invariants(const std::vector<PlayerResult>& players,
                                 std::size_t lobby_size) {
    InvariantReport report;
    report.players = players.size();
    const auto problem = [&report](std::string text) {
        if (report.problems.size() < InvariantReport::kMaxProblems) {
            report.problems.push_back(std::move(text));
        } else {
            ++report.problems_not_shown;
        }
    };

    std::unordered_map<std::string, const PlayerResult*> by_id;
    for (const PlayerResult& player : players) {
        by_id.emplace(player.player.id, &player);
    }

    // 1. Each player's own answer.
    std::set<std::string> in_two_places;
    for (const PlayerResult& player : players) {
        if (!player.enqueued) {
            ++report.never_enqueued;
            continue;
        }
        switch (player.status) {
            case FinalStatus::kInLobby:
            case FinalStatus::kQueued:
                break;
            case FinalStatus::kNotFound:
                ++report.lost;
                problem(player.player.id + " was enqueued but the cluster has never heard of them");
                break;
            case FinalStatus::kUnknown:
            case FinalStatus::kNeverEnqueued:
                ++report.lost;
                problem(player.player.id + " was enqueued but nobody could say where they are");
                break;
        }
    }

    // 2. Each lobby, as described by everyone who reported being in it.
    std::map<std::string, std::vector<const PlayerResult*>> reporters;
    for (const PlayerResult& player : players) {
        if (player.enqueued && player.status == FinalStatus::kInLobby) {
            reporters[player.lobby_id].push_back(&player);
        }
    }
    for (const auto& [lobby_id, who] : reporters) {
        const std::vector<std::string> members = sorted(who.front()->lobby_players);
        bool bad = false;

        if (members.size() != lobby_size) {
            problem("lobby " + lobby_id + " has " + std::to_string(members.size()) +
                    " players, not " + std::to_string(lobby_size));
            bad = true;
        }
        for (const PlayerResult* reporter : who) {
            const std::vector<std::string> theirs = sorted(reporter->lobby_players);
            if (theirs == members) {
                continue;
            }
            const bool disjoint = std::ranges::none_of(theirs, [&](const std::string& id) {
                return std::ranges::binary_search(members, id);
            });
            problem((disjoint ? "two different lobbies share the ID " + lobby_id + ": "
                              : "lobby " + lobby_id + ": ") +
                    who.front()->player.id + " says [" + join(members) + "] but " +
                    reporter->player.id + " says [" + join(theirs) + "]");
            bad = true;
            break;
        }

        std::set<std::string> queues;
        for (const std::string& member : members) {
            const auto found = by_id.find(member);
            if (found == by_id.end()) {
                problem("lobby " + lobby_id + " contains " + member + ", who was never sent");
                bad = true;
                continue;
            }
            const PlayerResult& them = *found->second;
            queues.insert(core::to_string(
                core::make_queue_key(them.player.region, them.player.mode, them.player.rating)));
            if (!them.enqueued) {
                ++report.ghosts;
                problem(member + "'s enqueue was reported as failed, yet they are in lobby " +
                        lobby_id);
            } else if (them.status == FinalStatus::kQueued) {
                in_two_places.insert(member);
                problem(member + " is in lobby " + lobby_id + " and also still queued");
            } else if (them.status == FinalStatus::kInLobby && them.lobby_id != lobby_id) {
                in_two_places.insert(member);
                problem(member + " is in lobby " + lobby_id + " and in lobby " + them.lobby_id);
            }
        }
        if (queues.size() > 1) {
            problem("lobby " + lobby_id + " mixes players from different queues");
            bad = true;
        }
        if (bad) {
            ++report.bad_lobbies;
        }
    }
    report.in_two_places = in_two_places.size();

    // 3. Tally the players who are where they should be.
    for (const PlayerResult& player : players) {
        if (!player.enqueued || in_two_places.contains(player.player.id)) {
            continue;
        }
        if (player.status == FinalStatus::kInLobby) {
            ++report.in_one_lobby;
        } else if (player.status == FinalStatus::kQueued) {
            ++report.queued;
        }
    }
    return report;
}

}  // namespace matchmesh::loadgen
