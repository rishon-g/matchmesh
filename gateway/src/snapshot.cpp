#include "matchmesh/gateway/snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

#include <crow/json.h>

#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/types.hpp"
#include "matchmesh/node/convert.hpp"

namespace matchmesh::gateway {
namespace {

using crow::json::wvalue;

constexpr double kRingSize = 18446744073709551616.0;  // 2^64: the ring's circumference

double fraction_round(std::uint64_t position) {
    return static_cast<double>(position) / kRingSize;
}

std::int64_t millis(const google::protobuf::Timestamp& ts) {
    return ts.seconds() * 1000 + ts.nanos() / 1'000'000;
}

struct Shard {
    std::string reporter;
    v1::ShardAssignment assignment;
};

}  // namespace

std::string build_snapshot(const std::vector<NodePoll>& polls, std::size_t max_lobbies) {
    // ---- Membership: the newest view anyone reported ----
    const v1::GetTopologyResponse* view = nullptr;
    for (const NodePoll& poll : polls) {
        if (poll.topology && (view == nullptr || poll.topology->epoch() > view->epoch())) {
            view = &*poll.topology;
        }
    }
    std::set<std::string> members;
    if (view != nullptr) {
        for (const auto& node : view->nodes()) {
            if (node.alive()) {
                members.insert(node.node_id());
            }
        }
    }
    core::HashRing ring;
    for (const std::string& member : members) {
        ring.add_node(member);
    }

    // ---- Shards: each queue once ----
    std::map<std::string, Shard> shards;  // by queue name
    for (const NodePoll& poll : polls) {
        if (!poll.topology) {
            continue;
        }
        for (const v1::ShardAssignment& assignment : poll.topology->shards()) {
            const std::string name = core::to_string(node::from_proto(assignment.queue()));
            const auto [it, inserted] = shards.try_emplace(name, Shard{poll.id, assignment});
            if (!inserted && ring.owner(node::from_proto(assignment.queue())) == poll.id) {
                it->second = Shard{poll.id, assignment};  // mid-move: trust the ring's owner
            }
        }
    }
    std::vector<std::pair<double, wvalue>> shard_list;
    std::map<std::string, std::pair<std::size_t, std::uint64_t>> per_node;  // queues, players
    std::uint64_t queued_total = 0;
    for (const auto& [name, shard] : shards) {
        const v1::QueueKey& key = shard.assignment.queue();
        const double at = fraction_round(core::HashRing::position(node::from_proto(key)));
        wvalue entry;
        entry["queue"] = name;
        entry["region"] = key.region();
        entry["mode"] = key.game_mode();
        entry["bracket"] = key.skill_bracket();
        entry["primary"] = shard.assignment.primary_node_id();
        entry["backup"] = shard.assignment.backup_node_id();
        entry["queued"] = shard.assignment.queued_players();
        entry["at"] = at;
        shard_list.emplace_back(at, std::move(entry));
        auto& [queues, players] = per_node[shard.assignment.primary_node_id()];
        ++queues;
        players += shard.assignment.queued_players();
        queued_total += shard.assignment.queued_players();
    }
    std::ranges::sort(shard_list, {}, &std::pair<double, wvalue>::first);

    // ---- Lobbies: newest first, each once ----
    struct Formed {
        std::int64_t at;
        const v1::Lobby* lobby;
        std::string node;
    };
    std::vector<Formed> formed;
    std::set<std::string> seen;
    for (const NodePoll& poll : polls) {
        if (!poll.topology) {
            continue;
        }
        for (const v1::Lobby& lobby : poll.topology->recent_lobbies()) {
            if (seen.insert(lobby.lobby_id()).second) {
                formed.push_back(Formed{millis(lobby.formed_at()), &lobby, poll.id});
            }
        }
    }
    std::ranges::sort(formed, [](const Formed& a, const Formed& b) {
        return a.at != b.at ? a.at > b.at : a.lobby->lobby_id() > b.lobby->lobby_id();
    });
    if (formed.size() > max_lobbies) {
        formed.resize(max_lobbies);
    }

    // ---- The document ----
    wvalue doc;
    doc["epoch"] = view != nullptr ? view->epoch() : 0;
    doc["coordinator"] = view != nullptr ? view->coordinator_node_id() : "";
    doc["members"] = members.size();
    doc["queued_players"] = queued_total;

    std::vector<wvalue> nodes;
    for (const NodePoll& poll : polls) {
        wvalue entry;
        entry["id"] = poll.id;
        entry["address"] = poll.address;
        entry["reachable"] = poll.topology.has_value();
        entry["member"] = members.contains(poll.id);
        const auto counts = per_node.find(poll.id);
        entry["queues"] = counts == per_node.end() ? 0 : counts->second.first;
        entry["queued"] = counts == per_node.end() ? 0 : counts->second.second;
        nodes.push_back(std::move(entry));
    }
    doc["nodes"] = std::move(nodes);

    std::vector<wvalue> points;
    for (const auto& point : ring.points()) {
        wvalue entry;
        entry["node"] = point.node;
        entry["at"] = fraction_round(point.position);
        points.push_back(std::move(entry));
    }
    doc["ring"] = std::move(points);

    std::vector<wvalue> shard_values;
    for (auto& [at, entry] : shard_list) {
        shard_values.push_back(std::move(entry));
    }
    doc["shards"] = std::move(shard_values);

    std::vector<wvalue> lobbies;
    for (const Formed& item : formed) {
        wvalue entry;
        entry["id"] = item.lobby->lobby_id();
        entry["queue"] = core::to_string(node::from_proto(item.lobby->queue()));
        entry["node"] = item.node;
        entry["formed_at"] = item.at;
        std::vector<wvalue> players;
        for (const v1::Player& player : item.lobby->players()) {
            wvalue p;
            p["id"] = player.player_id();
            p["rating"] = player.rating();
            players.push_back(std::move(p));
        }
        entry["players"] = std::move(players);
        lobbies.push_back(std::move(entry));
    }
    doc["lobbies"] = std::move(lobbies);
    return doc.dump();
}

}  // namespace matchmesh::gateway
