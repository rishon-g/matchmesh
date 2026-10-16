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

#include "matchmesh/loadgen/histogram.hpp"
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

enum class FinalStatus {
    kInLobby,
    kQueued,         // still waiting when the run ended
    kNotFound,       // the cluster says it has never heard of them
    kNeverEnqueued,  // every enqueue attempt failed
    kUnknown,        // enqueued, but the last status check failed
};

struct PlayerResult {
    PlannedPlayer player;
    bool enqueued = false;
    bool already_existed = false;  // a retry found the first attempt had worked
    int attempts = 0;
    std::string last_error;  // when never enqueued
    std::string served_by;   // the node that held their queue
    std::optional<std::chrono::system_clock::time_point> enqueued_at;  // server time
    FinalStatus status = FinalStatus::kUnknown;
    std::string lobby_id;
    std::vector<std::string> lobby_players;
    std::optional<std::chrono::system_clock::time_point> formed_at;  // server time
};

struct RunResult {
    std::vector<PlayerResult> players;  // same order as the plan
    LatencyHistogram enqueue_latency;
    LatencyHistogram time_to_lobby;
    std::chrono::nanoseconds send_duration{0};   // start -> last enqueue reply
    std::chrono::nanoseconds drain_duration{0};  // then waiting for lobbies
    std::size_t retries = 0;                     // enqueue attempts beyond the first
    std::map<std::string, std::size_t> served_by;  // node -> players it held

    [[nodiscard]] std::size_t count(FinalStatus status) const;
    [[nodiscard]] std::size_t lobbies() const;  // distinct lobby IDs seen
};

// Runs the whole test. Blocks until it's done.
[[nodiscard]] RunResult run_load(const std::vector<PlannedPlayer>& plan, const RunConfig& config);

}  // namespace matchmesh::loadgen
