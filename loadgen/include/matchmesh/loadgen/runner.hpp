#pragma once

// The load test itself: sends every planned player to the cluster at their
// scheduled moment, then waits for them to be placed in lobbies, and reports
// how long both took.
//
// Open loop. Players are sent when the schedule says, whether or not earlier
// calls have returned, and each enqueue's latency is measured from the
// player's *scheduled* arrival, not from when a thread got round to sending
// it. A closed-loop tester (send, wait, send the next) quietly slows down
// whenever the server does, so a 2-second stall shows up as one slow sample
// instead of the hundreds of players who were kept waiting behind it. This
// mistake is called coordinated omission; measuring from the schedule avoids it.
//
// Two numbers per player:
//   * enqueue latency: scheduled arrival -> the cluster's reply. What a
//     player waits for "you're in the queue". Includes forwarding,
//     replication to the backup, and any retries.
//   * time to lobby: the server's enqueued_at -> the lobby's formed_at, both
//     server timestamps. How long matchmaking took; this depends on how many
//     similar players are around, not on server speed.
//
// Like a real client it retries UNAVAILABLE (a node down, a queue moving),
// trying the next target node each time, until a per-player deadline.

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "matchmesh/loadgen/results.hpp"
#include "matchmesh/loadgen/workload.hpp"

namespace matchmesh::loadgen {

struct RunConfig {
    // host:port of the nodes to send to. Player i goes to targets[i % n]
    // (any node accepts any request), moving on to the next on a retry.
    std::vector<std::string> targets;
    std::size_t threads = 32;
    std::chrono::milliseconds rpc_timeout{2'000};    // one attempt
    std::chrono::milliseconds give_up_after{10'000};  // one player, all retries
    // After sending: keep asking about players not yet in a lobby until all
    // are, or nobody has been placed for `drain_quiet`, or `drain_timeout`.
    std::chrono::milliseconds drain_timeout{60'000};
    std::chrono::milliseconds drain_quiet{5'000};
    std::chrono::milliseconds poll_interval{500};
    // Optional progress messages ("sent 5,000 of 10,000", ...).
    std::function<void(std::string_view)> progress;
};

// Runs the whole test. Blocks until it's done.
[[nodiscard]] RunResult run_load(const std::vector<PlannedPlayer>& plan, const RunConfig& config);

}  // namespace matchmesh::loadgen
