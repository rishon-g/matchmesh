#pragma once

// Workload: the players a load test sends, and when each one arrives.
//
// Built to look like real traffic rather than a uniform stream:
//   * Arrivals are a Poisson process: the gaps between players are random
//     (exponentially distributed) around the target rate, so there are
//     natural bursts and lulls, the way independent players actually show up.
//   * Ratings follow a bell curve (normal distribution, mean 1500, sd 300,
//     clamped to 0-3000), as Elo-style ratings do: most players are average,
//     few are very weak or very strong. That makes some queues crowded and
//     others sparse, which is exactly what stresses a matchmaker.
//   * Regions and game modes are drawn by weight (more players in us-west
//     than asia, more ranked than casual).
//
// Everything comes from one seeded random generator, so the same seed gives
// the same workload every time and runs can be compared fairly.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace matchmesh::loadgen {

struct WorkloadConfig {
    std::size_t players = 10'000;
    // Mean arrival rate. 0 means everyone arrives at once (a pure burst).
    double arrivals_per_second = 1'000.0;

    double rating_mean = 1'500.0;
    double rating_stddev = 300.0;
    int rating_min = 0;
    int rating_max = 3'000;

    // (name, weight) pairs; weights needn't add up to 1.
    std::vector<std::pair<std::string, double>> regions = {
        {"us-west", 0.30}, {"us-east", 0.30}, {"eu", 0.25}, {"asia", 0.15}};
    std::vector<std::pair<std::string, double>> modes = {{"ranked", 0.60}, {"casual", 0.40}};

    std::uint64_t seed = 42;
    std::string id_prefix = "p";  // player IDs are prefix + number: p0, p1, ...
};

struct PlannedPlayer {
    std::string id;
    int rating = 0;
    std::string region;
    std::string mode;
    std::chrono::microseconds arrives_at{0};  // after the start of the run
};

// The players in arrival order. Throws std::invalid_argument for a config
// that makes no sense (negative rate, empty or zero-weight region list, ...).
[[nodiscard]] std::vector<PlannedPlayer> make_workload(const WorkloadConfig& config);

}  // namespace matchmesh::loadgen
