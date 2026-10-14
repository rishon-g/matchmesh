#pragma once

// Router: knows every node in the cluster, which one owns each queue, and how
// to reach the others.
//
// Every node builds the same Router from the same peer list, so they all
// agree on owners without talking to each other (see core::HashRing).
// Membership is fixed for now: failure detection and changing membership
// come in Phase 5.
//
// Immutable after construction, so it is safe to use from any number of
// gRPC handler threads at once with no locking. (gRPC stubs are themselves
// thread-safe.)

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/types.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::node {

// Parses a peer list such as "node-a=node-a:50051,node-b=node-b:50051" into
// {node ID -> address}. Spaces around entries are ignored and an empty string
// means no peers. Throws std::invalid_argument for a malformed entry or a
// node listed twice.
[[nodiscard]] std::map<std::string, std::string> parse_peers(std::string_view spec);

class Router {
public:
    // `peers` maps node ID -> host:port. It may include this node itself
    // (handy: every node can be given the same list); that entry is ignored.
    // `forward_timeout` caps how long a forwarded call may take.
    Router(std::string self_id, const std::map<std::string, std::string>& peers,
           std::chrono::milliseconds forward_timeout = std::chrono::seconds(2));

    [[nodiscard]] const std::string& self_id() const noexcept;

    // The node that owns this queue. Never empty: this node is always on the ring.
    [[nodiscard]] std::string owner_of(const core::QueueKey& queue) const;
    [[nodiscard]] bool is_local(const core::QueueKey& queue) const;

    // Client stub for another node. Throws std::out_of_range for an unknown ID.
    [[nodiscard]] v1::MatchmakingService::StubInterface& stub_for(const std::string& node_id) const;

    [[nodiscard]] std::chrono::milliseconds forward_timeout() const noexcept;
    [[nodiscard]] const core::HashRing& ring() const noexcept;
    // The other nodes and their addresses.
    [[nodiscard]] const std::map<std::string, std::string>& peers() const noexcept;

private:
    std::string self_id_;
    std::map<std::string, std::string> peers_;  // excludes self
    core::HashRing ring_;                       // includes self
    std::chrono::milliseconds forward_timeout_;
    std::unordered_map<std::string, std::unique_ptr<v1::MatchmakingService::StubInterface>> stubs_;
};

}  // namespace matchmesh::node
