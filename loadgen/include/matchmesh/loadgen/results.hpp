#pragma once

// What a load test found out about each player, and the run as a whole.
// Plain data, no networking: the runner fills it in, the report and the
// invariant checker read it.

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "matchmesh/loadgen/histogram.hpp"
#include "matchmesh/loadgen/workload.hpp"

namespace matchmesh::loadgen {

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
    // Scheduled arrival to the final enqueue reply (when enqueued).
    std::chrono::nanoseconds latency{0};
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
    [[nodiscard]] std::size_t players_retried() const;  // needed more than one attempt
};

}  // namespace matchmesh::loadgen
