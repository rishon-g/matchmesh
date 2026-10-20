// matchmesh_loadgen — simulates thousands of players joining a MatchMesh
// cluster and reports how fast it responded and how long matching took.
//
//   matchmesh_loadgen --targets node-a:50051,node-b:50051 [--players 10000]
//                     [--rate 1000] [--threads 32] [--seed 42] [--json out.json]
//
//   --targets  nodes to send to, comma-separated (or MATCHMESH_TARGETS)
//   --players  how many players to simulate                  (default 10000)
//   --rate     mean arrivals per second, Poisson; 0 = all at once (default 1000)
//   --threads  client threads sending in parallel             (default 32)
//   --seed     random seed; the same seed gives the same players (default 42)
//   --json     also write the results to this file, for scripts
//
// Afterwards it runs the lost-player invariant checker (see invariants.hpp):
// everyone who was enqueued must be in exactly one lobby or still queued.
// Exits 0 if every player was enqueued and every invariant holds, 1 otherwise.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "matchmesh/core/version.hpp"
#include "matchmesh/loadgen/histogram.hpp"
#include "matchmesh/loadgen/invariants.hpp"
#include "matchmesh/loadgen/runner.hpp"
#include "matchmesh/loadgen/workload.hpp"

namespace {

using matchmesh::loadgen::FinalStatus;
using matchmesh::loadgen::InvariantReport;
using matchmesh::loadgen::format_duration;
using matchmesh::loadgen::LatencyHistogram;
using matchmesh::loadgen::RunResult;

struct Options {
    std::vector<std::string> targets;
    matchmesh::loadgen::WorkloadConfig workload;
    std::size_t threads = 32;
    std::string json_path;
};

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view part =
            text.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                               : comma - start);
        if (!part.empty()) {
            parts.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

bool parse_args(int argc, char** argv, Options& out) {
    if (const char* env = std::getenv("MATCHMESH_TARGETS"); env != nullptr) {
        out.targets = split(env);
    }
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view flag = argv[i];
            if (i + 1 >= argc) {
                return false;
            }
            const std::string value = argv[++i];
            if (flag == "--targets") {
                out.targets = split(value);
            } else if (flag == "--players") {
                out.workload.players = std::stoul(value);
            } else if (flag == "--rate") {
                out.workload.arrivals_per_second = std::stod(value);
            } else if (flag == "--threads") {
                out.threads = std::stoul(value);
            } else if (flag == "--seed") {
                out.workload.seed = std::stoull(value);
            } else if (flag == "--json") {
                out.json_path = value;
            } else {
                return false;
            }
        }
    } catch (const std::exception&) {
        return false;  // a number that wasn't one
    }
    return !out.targets.empty() && out.threads > 0;
}

std::string with_commas(std::size_t n) {
    std::string digits = std::to_string(n);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return digits;
}

double seconds(std::chrono::nanoseconds d) {
    return std::chrono::duration<double>(d).count();
}

double ms(std::chrono::microseconds d) {
    return static_cast<double>(d.count()) / 1'000.0;
}

void print_histogram(const char* title, const LatencyHistogram& h) {
    std::cout << title << " (n=" << with_commas(h.count()) << ")\n";
    if (h.count() == 0) {
        std::cout << "  no samples\n";
        return;
    }
    std::cout << "  p50 " << format_duration(h.percentile(50)) << "   p90 "
              << format_duration(h.percentile(90)) << "   p99 " << format_duration(h.percentile(99))
              << "   p99.9 " << format_duration(h.percentile(99.9)) << "   max "
              << format_duration(h.max()) << "   mean " << format_duration(h.mean()) << "\n";
    std::uint64_t biggest = 0;
    for (const auto& range : h.ranges()) {
        biggest = std::max(biggest, range.count);
    }
    for (const auto& range : h.ranges()) {
        const auto bar = static_cast<std::size_t>(40 * range.count / biggest);
        char line[160];
        std::snprintf(line, sizeof line, "  %9s - %-9s %7llu %s\n",
                      format_duration(range.low).c_str(), format_duration(range.high).c_str(),
                      static_cast<unsigned long long>(range.count),
                      std::string(bar == 0 && range.count > 0 ? 1 : bar, '#').c_str());
        std::cout << line;
    }
}

std::string histogram_json(const LatencyHistogram& h) {
    std::ostringstream out;
    out << "{\"count\": " << h.count() << ", \"p50_ms\": " << ms(h.percentile(50))
        << ", \"p90_ms\": " << ms(h.percentile(90)) << ", \"p99_ms\": " << ms(h.percentile(99))
        << ", \"p999_ms\": " << ms(h.percentile(99.9)) << ", \"max_ms\": " << ms(h.max())
        << ", \"mean_ms\": " << ms(h.mean()) << "}";
    return out.str();
}

void write_json(const std::string& path, const Options& options, const RunResult& run,
                const InvariantReport& invariants) {
    std::ofstream out(path);
    out << "{\n"
        << "  \"players\": " << run.players.size() << ",\n"
        << "  \"target_rate\": " << options.workload.arrivals_per_second << ",\n"
        << "  \"achieved_rate\": "
        << static_cast<double>(run.players.size()) / seconds(run.send_duration) << ",\n"
        << "  \"seed\": " << options.workload.seed << ",\n"
        << "  \"send_seconds\": " << seconds(run.send_duration) << ",\n"
        << "  \"retries\": " << run.retries << ",\n"
        << "  \"players_retried\": " << run.players_retried() << ",\n"
        << "  \"enqueue_latency\": " << histogram_json(run.enqueue_latency) << ",\n"
        << "  \"time_to_lobby\": " << histogram_json(run.time_to_lobby) << ",\n"
        << "  \"final\": {\"in_lobby\": " << run.count(FinalStatus::kInLobby)
        << ", \"queued\": " << run.count(FinalStatus::kQueued)
        << ", \"not_found\": " << run.count(FinalStatus::kNotFound)
        << ", \"never_enqueued\": " << run.count(FinalStatus::kNeverEnqueued)
        << ", \"unknown\": " << run.count(FinalStatus::kUnknown)
        << ", \"lobbies\": " << run.lobbies() << "},\n"
        << "  \"invariants\": {\"ok\": " << (invariants.ok() ? "true" : "false")
        << ", \"in_one_lobby\": " << invariants.in_one_lobby << ", \"queued\": " << invariants.queued
        << ", \"lost\": " << invariants.lost << ", \"in_two_places\": " << invariants.in_two_places
        << ", \"bad_lobbies\": " << invariants.bad_lobbies << ", \"ghosts\": " << invariants.ghosts
        << "},\n"
        << "  \"served_by\": {";
    bool first = true;
    for (const auto& [node, count] : run.served_by) {
        out << (first ? "" : ", ") << "\"" << node << "\": " << count;
        first = false;
    }
    out << "}\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        std::cerr << "usage: matchmesh_loadgen --targets <host:port,...> [--players N] "
                     "[--rate per_second] [--threads N] [--seed N] [--json path]\n"
                     "       (--targets may instead come from MATCHMESH_TARGETS)\n";
        return EXIT_FAILURE;
    }

    const auto plan = matchmesh::loadgen::make_workload(options.workload);
    std::cout << "matchmesh_loadgen v" << matchmesh::core::version() << ": "
              << with_commas(plan.size()) << " players, Poisson arrivals at "
              << options.workload.arrivals_per_second << "/s (seed " << options.workload.seed
              << "), " << options.threads << " client threads, " << options.targets.size()
              << " target node(s)" << std::endl;

    matchmesh::loadgen::RunConfig config;
    config.targets = options.targets;
    config.threads = options.threads;
    config.progress = [](std::string_view message) {
        std::cout << "  ... " << message << std::endl;
    };
    const RunResult run = matchmesh::loadgen::run_load(plan, config);

    const std::size_t failed = run.count(FinalStatus::kNeverEnqueued);
    std::cout << "\nSent " << with_commas(plan.size() - failed) << " of "
              << with_commas(plan.size()) << " in " << seconds(run.send_duration) << " s ("
              << static_cast<std::size_t>(static_cast<double>(plan.size()) /
                                          seconds(run.send_duration))
              << " players/s); " << with_commas(failed) << " failed; "
              << with_commas(run.retries) << " retries ("
              << with_commas(run.players_retried()) << " players needed more than one try)\n\n";
    print_histogram("Enqueue latency, from scheduled arrival to reply", run.enqueue_latency);
    std::cout << "\n";
    print_histogram("Time to lobby, server enqueue to lobby formed", run.time_to_lobby);

    std::cout << "\nAfter " << seconds(run.drain_duration) << " s waiting for lobbies:\n"
              << "  in a lobby      " << with_commas(run.count(FinalStatus::kInLobby)) << " ("
              << with_commas(run.lobbies()) << " lobbies)\n"
              << "  still queued    " << with_commas(run.count(FinalStatus::kQueued)) << "\n"
              << "  not found       " << with_commas(run.count(FinalStatus::kNotFound)) << "\n"
              << "  never enqueued  " << with_commas(failed) << "\n"
              << "  unknown         " << with_commas(run.count(FinalStatus::kUnknown)) << "\n"
              << "Queues held by:";
    for (const auto& [node, count] : run.served_by) {
        std::cout << "  " << node << " " << with_commas(count);
    }
    std::cout << std::endl;

    const InvariantReport invariants = matchmesh::loadgen::check_invariants(run.players);
    std::cout << "\nInvariants: " << (invariants.ok() ? "OK" : "VIOLATED") << ". "
              << with_commas(invariants.in_one_lobby) << " in exactly one lobby, "
              << with_commas(invariants.queued) << " still queued, " << invariants.lost
              << " lost, " << invariants.in_two_places << " in two places, "
              << invariants.bad_lobbies << " bad lobbies, " << invariants.ghosts << " ghosts"
              << std::endl;
    for (const std::string& problem : invariants.problems) {
        std::cout << "  ! " << problem << "\n";
    }
    if (invariants.problems_not_shown > 0) {
        std::cout << "  ! ... and " << invariants.problems_not_shown << " more\n";
    }

    if (!options.json_path.empty()) {
        write_json(options.json_path, options, run, invariants);
        std::cout << "Wrote " << options.json_path << std::endl;
    }
    return failed == 0 && invariants.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
