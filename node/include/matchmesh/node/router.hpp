#pragma once

// Router: knows every node that may be in the cluster, which one owns each
// queue *right now*, and how to reach the others.
//
// Ownership follows the agreed membership view (see core::Membership). When a
// new epoch arrives, apply_view() builds a fresh hash ring from that view's
// members and swaps it in. Requests already in flight keep the ring they
// started with; new requests see the new one. Because every node applies the
// same views, they all agree on owners once they have seen the same epoch.
//
// Thread-safe: any number of gRPC handler threads read it while the cluster
// manager applies new views. Reads take a short lock only to copy a
// shared_ptr to the current routing table; the ring itself is never modified
// after it's built, so it can be used without any lock.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/core/types.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

#include <grpcpp/grpcpp.h>

namespace matchmesh::node {

// Parses a peer list such as "node-a=node-a:50051,node-b=node-b:50051" into
// {node ID -> address}. Spaces around entries are ignored and an empty string
// means no peers. Throws std::invalid_argument for a malformed entry or a
// node listed twice.
[[nodiscard]] std::map<std::string, std::string> parse_peers(std::string_view spec);

// Opens a channel to another node, tuned for a cluster where peers die and
// come back. After a failed connection gRPC waits before trying again, by
// default 1 s growing 1.6x per failure up to 2 minutes, so a node that was
// down for a while could stay unreachable for a long time after it returned.
// Between nodes we retry quickly instead: start at 100 ms, never wait more
// than 1 s.
[[nodiscard]] std::shared_ptr<grpc::Channel> make_peer_channel(const std::string& address);

// Who owns a queue, and under which membership epoch that was decided.
struct RouteDecision {
    std::string owner;
    std::uint64_t epoch = 0;
};

class Router {
public:
    // `peers` maps node ID -> host:port for every *other* node that may join
    // (an entry for this node itself is ignored, so every node can be given
    // the same list). Initially all of them are members, epoch 0.
    // `forward_timeout` caps how long a forwarded call may take.
    Router(std::string self_id, const std::map<std::string, std::string>& peers,
           std::chrono::milliseconds forward_timeout = std::chrono::seconds(2));

    [[nodiscard]] const std::string& self_id() const noexcept;

    // The node that owns this queue under the current view. Never empty.
    [[nodiscard]] RouteDecision route(const core::QueueKey& queue) const;
    [[nodiscard]] std::string owner_of(const core::QueueKey& queue) const;
    [[nodiscard]] bool is_local(const core::QueueKey& queue) const;

    // Switches to `view` if it is newer than the current one. Members this
    // node has never heard of are skipped (they have no address). Returns
    // true if the routing table changed.
    bool apply_view(const core::MembershipView& view);

    [[nodiscard]] std::uint64_t epoch() const;
    [[nodiscard]] std::vector<std::string> members() const;  // current ring's nodes, sorted

    // Client stub for another node. Throws std::out_of_range for an unknown ID.
    [[nodiscard]] v1::MatchmakingService::StubInterface& stub_for(const std::string& node_id) const;

    [[nodiscard]] std::chrono::milliseconds forward_timeout() const noexcept;
    // Every other node this node knows about, and its address.
    [[nodiscard]] const std::map<std::string, std::string>& peers() const noexcept;

private:
    // One immutable routing table: built once per view, then only read.
    struct Table {
        core::MembershipView view;
        core::HashRing ring;
    };

    [[nodiscard]] std::shared_ptr<const Table> table() const;

    const std::string self_id_;
    std::map<std::string, std::string> peers_;  // excludes self; fixed after construction
    const std::chrono::milliseconds forward_timeout_;
    std::unordered_map<std::string, std::unique_ptr<v1::MatchmakingService::StubInterface>> stubs_;

    mutable std::mutex mutex_;          // guards table_ (the pointer, not the table)
    std::shared_ptr<const Table> table_;
};

}  // namespace matchmesh::node
