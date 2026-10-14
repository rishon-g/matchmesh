#pragma once

// Membership: which nodes are alive, who the coordinator is, and the agreed
// membership list ("view") that routing follows.
//
// Two layers, on purpose:
//
//  1. Local failure detection. Every node tracks when it last heard from each
//     other node (any heartbeat, sent or received). A node silent for longer
//     than `suspect_after` (1.5 s) is *suspected* dead. Each node's
//     suspicions are its own opinion and may briefly differ from others'.
//
//  2. The agreed view. Only the coordinator, the alive node with the lowest
//     ID, turns its opinion into a published view: the sorted member list
//     plus an epoch number that goes up by one on every change. Views spread
//     through heartbeats, and every node adopts any view newer than its own.
//     Routing follows the view, never a node's private suspicions, so all
//     nodes route the same way once they've seen the same epoch.
//
// Pure logic with no networking or clock of its own: callers pass the current
// time in, which keeps it fully testable. Thread-safe (one internal mutex):
// heartbeat threads, gRPC handlers and a ticker all call into it.

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

// One agreed membership list.
struct MembershipView {
    std::uint64_t epoch = 0;
    std::vector<std::string> members;  // sorted by ID

    // The node that published this view: always its lowest member, because a
    // coordinator includes itself and is the lowest alive node.
    [[nodiscard]] std::string coordinator() const;

    bool operator==(const MembershipView&) const = default;
};

// Whether `candidate` should replace `current`: a higher epoch wins. If two
// coordinators ever published the same epoch (only possible during a network
// split), the one with the lower coordinator ID wins, so every node still
// picks the same view.
[[nodiscard]] bool is_newer(const MembershipView& candidate, const MembershipView& current);

struct MembershipConfig {
    std::chrono::milliseconds suspect_after{1500};  // silence before a node is suspected dead
};

class Membership {
public:
    // `all_nodes` is every node that may ever be a member, including `self`
    // (the static peer list). Everyone starts out alive and in the epoch-0
    // view; silence over the next `suspect_after` is what removes a node.
    // Throws std::invalid_argument if `self` is empty or not in `all_nodes`.
    Membership(std::string self, std::vector<std::string> all_nodes, TimePoint now,
               MembershipConfig config = {});

    // We heard from `node` (a heartbeat in either direction). Unknown nodes
    // and ourselves are ignored.
    void record_contact(const std::string& node, TimePoint now);

    // Another node told us its view. Adopted if newer than ours. Returns true
    // if it was adopted.
    bool observe(const MembershipView& view);

    // Re-evaluates who is alive as of `now`. If we are the coordinator and the
    // alive set differs from the current view, publishes a new view with the
    // next epoch and returns it. Otherwise returns nothing.
    std::optional<MembershipView> tick(TimePoint now);

    [[nodiscard]] MembershipView view() const;
    // Our own opinion (failure detection), not the agreed view.
    [[nodiscard]] bool is_alive(const std::string& node, TimePoint now) const;
    [[nodiscard]] std::vector<std::string> alive(TimePoint now) const;  // sorted, includes self
    [[nodiscard]] std::string local_coordinator(TimePoint now) const;    // lowest alive

    [[nodiscard]] const std::string& self() const noexcept;
    [[nodiscard]] const MembershipConfig& config() const noexcept;
    [[nodiscard]] const std::vector<std::string>& all_nodes() const noexcept;  // sorted

private:
    [[nodiscard]] bool is_alive_locked(const std::string& node, TimePoint now) const;
    [[nodiscard]] std::vector<std::string> alive_locked(TimePoint now) const;

    const std::string self_;
    const std::vector<std::string> all_nodes_;
    const MembershipConfig config_;

    mutable std::mutex mutex_;  // guards everything below
    std::map<std::string, TimePoint> last_contact_;
    MembershipView view_;
};

}  // namespace matchmesh::core
