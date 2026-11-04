#pragma once

// A whole MatchMesh cluster in one process, for the in-browser demo.
//
// Five simulated nodes run the real core: core::Membership for failure
// detection and views, core::MatchmakingEngine for the queues and matching,
// core::BackupReplicator and core::ShardCoordinator for replication,
// hand-over and take-over. Only the network and the clock are simulated:
//
//   * Time is virtual. advance(ms) moves it forward in 10 ms steps, and
//     every timer the real node has (heartbeats every 500 ms, failure checks
//     every 100 ms, the shard manager's 100 ms retry pass, a match pass
//     every 50 ms) fires at the same virtual times it would for real.
//   * Messages between nodes are function calls that succeed only if both
//     nodes are up and on the same side of any network split.
//   * Players arrive at a set rate (Poisson). Each one is sent to a random
//     live node, forwarded to the queue's owner, and retried with backoff
//     on any refusal, with the same request ID, exactly like the load
//     generator. Clients can reach every live node, even during a split.
//
// No threads, no sockets, no wall clock: the same code runs natively for
// the tests and as WebAssembly in the browser.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace matchmesh::sim {

struct SimConfig {
    std::vector<std::string> nodes = {"node-a", "node-b", "node-c", "node-d", "node-e"};
    double arrivals_per_second = 25.0;
    std::uint64_t seed = 1;
    // Wall-clock time (ms since the Unix epoch) that virtual time 0 stands
    // for. Only used to stamp lobbies, so the dashboard can say "2 s ago".
    double unix_start_ms = 0.0;
};

// Checked across every live node's copies (primaries and backups).
struct Invariants {
    std::size_t acknowledged = 0;    // players told "you're in"
    std::size_t in_lobby = 0;        // ...now in a lobby
    std::size_t queued = 0;          // ...still waiting in a queue
    std::size_t lost = 0;            // ...held by no live node at all
    std::size_t in_two_lobbies = 0;  // ...in two different lobbies
    std::size_t contested_queues = 0;  // queues two nodes serve at once (split brain)
    std::size_t retries = 0;         // refused attempts clients retried
};

struct Event {
    std::uint64_t seq = 0;
    double at_ms = 0.0;
    std::string kind;  // "action", "view", "detect", "handover", "promote", "warn"
    std::string text;
};

class Cluster {
public:
    explicit Cluster(SimConfig config = {});
    ~Cluster();

    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;

    // Moves virtual time forward by `ms`, running everything due on the way.
    // Time runs in whole 10 ms steps; a remainder is kept for the next call.
    void advance(double ms);

    // Crash a node (its memory is gone) and start a fresh process for it.
    void kill(const std::string& node);
    void restart(const std::string& node);
    // Split the network into the first two nodes and the rest, and undo it.
    void split();
    void heal();
    void set_rate(double arrivals_per_second);

    [[nodiscard]] double now_ms() const;
    [[nodiscard]] bool is_up(const std::string& node) const;
    [[nodiscard]] bool is_split() const;
    [[nodiscard]] double rate() const;
    [[nodiscard]] const std::vector<std::string>& node_ids() const;

    [[nodiscard]] Invariants invariants() const;
    [[nodiscard]] std::vector<Event> events() const;  // the most recent, oldest first

    // The newest view any live node holds: its epoch and members.
    [[nodiscard]] std::uint64_t epoch() const;
    [[nodiscard]] std::vector<std::string> members() const;
    // Which node serves `queue_name` ("us-west/ranked/2") for clients right
    // now, per that view's ring, or "" if no live node is serving it.
    [[nodiscard]] std::string serving_node(const std::string& queue_name) const;

    // The dashboard's snapshot document (the same shape the gateway sends,
    // see dashboard/src/app/topology.ts), plus a "sim" section with the
    // invariants, events and controls' state.
    [[nodiscard]] std::string snapshot_json(std::size_t max_lobbies = 30) const;

    class Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace matchmesh::sim
