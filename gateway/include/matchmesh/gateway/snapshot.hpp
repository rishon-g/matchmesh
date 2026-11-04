#pragma once

// Snapshot: merges what every node said into one picture of the cluster, as
// the JSON document the dashboard draws.
//
// No single node knows everything: each reports the shared membership view
// but only the queues it is primary for and the lobbies it formed. So the
// gateway asks all of them and combines:
//
//   * membership: from whichever answer has the newest epoch;
//   * nodes: every configured node, whether the gateway could reach it, and
//     whether the view counts it as a member;
//   * ring: the hash ring's points (100 per member), as fractions of the way
//     round, computed the same way every node computes it;
//   * shards: each queue once, with its primary, backup, size and position
//     on the ring (if two nodes both claim one while it moves, the one the
//     ring names wins);
//   * lobbies: the most recently formed, newest first.
//
// Pure: no networking or clock, so the same answers always give the same
// JSON (which is how the poller tells whether anything changed).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "matchmesh/v1/cluster.pb.h"

namespace matchmesh::gateway {

// One node's answer to GetTopology in one round of polling.
struct NodePoll {
    std::string id;
    std::string address;
    std::optional<v1::GetTopologyResponse> topology;  // empty: didn't answer in time
};

inline constexpr std::size_t kSnapshotLobbies = 30;

[[nodiscard]] std::string build_snapshot(const std::vector<NodePoll>& polls,
                                         std::size_t max_lobbies = kSnapshotLobbies);

}  // namespace matchmesh::gateway
