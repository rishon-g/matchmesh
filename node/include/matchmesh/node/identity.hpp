#pragma once

// Identity of one run of a node process.
//
// A node keeps its name (node-c) across restarts, but each run is a new
// process with empty memory. The incarnation number tells runs apart: peers
// use it to spot a restart (see core::Membership), and lobby IDs include it
// so a restarted node can never hand out an ID its previous run already used.

#include <cstdint>
#include <string>

namespace matchmesh::node {

// A random, non-zero number for this run of the process.
[[nodiscard]] std::uint64_t new_incarnation();

// What lobby IDs from this run start with, e.g. "node-c-9f3c2a1e", so IDs
// look like "node-c-9f3c2a1e-17": the node, which run of it, then a counter.
[[nodiscard]] std::string lobby_id_prefix(const std::string& node_id, std::uint64_t incarnation);

}  // namespace matchmesh::node
