#include "matchmesh/loadgen/runner.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "matchmesh/core/types.hpp"
#include "matchmesh/node/convert.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::loadgen {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::system_clock;

std::chrono::system_clock::time_point from_timestamp(const google::protobuf::Timestamp& ts) {
    return system_clock::time_point(std::chrono::duration_cast<system_clock::duration>(
        std::chrono::seconds(ts.seconds()) + std::chrono::nanoseconds(ts.nanos())));
}

std::string served_by(const grpc::ClientContext& context) {
    const auto& metadata = context.GetServerInitialMetadata();
    const auto it = metadata.find(
        grpc::string_ref(node::kServedByKey.data(), node::kServedByKey.size()));
    return it == metadata.end() ? std::string{} : std::string(it->second.data(), it->second.size());
}

bool retryable(const grpc::Status& status) {
    return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
           status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
}

// One stub per target node, shared by every thread (stubs are thread-safe).
class Targets {
public:
    explicit Targets(const std::vector<std::string>& addresses) {
        if (addresses.empty()) {
            throw std::invalid_argument("at least one target is needed");
        }
        for (const std::string& address : addresses) {
            stubs_.push_back(v1::MatchmakingService::NewStub(node::make_peer_channel(address)));
        }
    }
    // The stub for player `index`'s `attempt`-th try: spread players over
    // the nodes, and move to the next node on every retry.
    v1::MatchmakingService::Stub& pick(std::size_t index, int attempt) const {
        return *stubs_[(index + static_cast<std::size_t>(attempt)) % stubs_.size()];
    }

private:
    std::vector<std::unique_ptr<v1::MatchmakingService::Stub>> stubs_;
};

// Runs `work(thread_index)` on `count` threads and waits for all of them.
template <typename Work>
void on_threads(std::size_t count, Work work) {
    std::vector<std::jthread> threads;
    threads.reserve(count);
    for (std::size_t t = 0; t < count; ++t) {
        threads.emplace_back([&work, t] { work(t); });
    }
}

// Sends one player, retrying while the cluster says "try again". Returns the
// time the final reply arrived.
Clock::time_point enqueue(const Targets& targets, std::size_t index, const RunConfig& config,
                          PlayerResult& result) {
    v1::EnqueueRequest request;
    request.mutable_player()->set_player_id(result.player.id);
    request.mutable_player()->set_rating(result.player.rating);
    request.set_region(result.player.region);
    request.set_game_mode(result.player.mode);

    const auto give_up = Clock::now() + config.give_up_after;
    auto backoff = std::chrono::milliseconds(20);
    for (int attempt = 0;; ++attempt) {
        grpc::ClientContext context;
        const auto attempt_ends = std::min(Clock::now() + config.rpc_timeout, give_up);
        context.set_deadline(system_clock::now() + (attempt_ends - Clock::now()));
        v1::EnqueueResponse response;
        const grpc::Status status = targets.pick(index, attempt).Enqueue(&context, request, &response);
        result.attempts = attempt + 1;

        if (status.ok()) {
            result.enqueued = true;
            result.served_by = served_by(context);
            result.enqueued_at = from_timestamp(response.enqueued_at());
            return Clock::now();
        }
        if (status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
            // An earlier attempt got through but its reply was lost.
            result.enqueued = true;
            result.already_existed = true;
            result.served_by = served_by(context);
            return Clock::now();
        }
        result.last_error = status.error_message();
        if (!retryable(status) || Clock::now() + backoff >= give_up) {
            return Clock::now();
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(250));
    }
}

// Asks where one player is. Returns true if their status changed to "in a lobby".
bool check(const Targets& targets, std::size_t index, int round, const RunConfig& config,
           PlayerResult& result) {
    v1::GetLobbyRequest request;
    request.set_player_id(result.player.id);
    node::to_proto(core::make_queue_key(result.player.region, result.player.mode,
                                        result.player.rating),
                   request.mutable_queue());
    grpc::ClientContext context;
    context.set_deadline(system_clock::now() + config.rpc_timeout);
    v1::GetLobbyResponse response;
    // A different node each round, so one dead node can't hide a player.
    if (!targets.pick(index, round).GetLobby(&context, request, &response).ok()) {
        result.status = FinalStatus::kUnknown;
        return false;
    }
    switch (response.status()) {
        case v1::PLAYER_STATUS_IN_LOBBY:
            result.status = FinalStatus::kInLobby;
            result.lobby_id = response.lobby().lobby_id();
            result.lobby_players.clear();
            for (const auto& player : response.lobby().players()) {
                result.lobby_players.push_back(player.player_id());
            }
            result.formed_at = from_timestamp(response.lobby().formed_at());
            return true;
        case v1::PLAYER_STATUS_QUEUED:
            result.status = FinalStatus::kQueued;
            return false;
        default:
            result.status = FinalStatus::kNotFound;
            return false;
    }
}

}  // namespace

RunResult run_load(const std::vector<PlannedPlayer>& plan, const RunConfig& config) {
    const Targets targets(config.targets);
    const std::size_t thread_count = std::max<std::size_t>(1, config.threads);
    RunResult run;
    run.players.reserve(plan.size());
    for (const PlannedPlayer& player : plan) {
        PlayerResult result;
        result.player = player;
        run.players.push_back(std::move(result));
    }

    // ---- 1. Send everyone on schedule ----
    // Thread t sends players t, t + T, t + 2T, ... so each thread's players
    // are in arrival order and the work is spread evenly. Each thread
    // records into its own histogram; they're merged at the end.
    std::vector<LatencyHistogram> latencies(thread_count);
    std::atomic<std::size_t> sent{0};
    const Clock::time_point start = Clock::now();
    on_threads(thread_count, [&](std::size_t t) {
        for (std::size_t i = t; i < plan.size(); i += thread_count) {
            const Clock::time_point scheduled = start + plan[i].arrives_at;
            std::this_thread::sleep_until(scheduled);
            const Clock::time_point replied = enqueue(targets, i, config, run.players[i]);
            if (run.players[i].enqueued) {
                run.players[i].latency = replied - scheduled;
                latencies[t].record(run.players[i].latency);  // failures are counted, not timed
            }
            if (const std::size_t done = ++sent; config.progress && done % 1'000 == 0) {
                config.progress("sent " + std::to_string(done) + " of " +
                                std::to_string(plan.size()));
            }
        }
    });
    run.send_duration = Clock::now() - start;
    for (const LatencyHistogram& histogram : latencies) {
        run.enqueue_latency.merge(histogram);
    }

    // ---- 2. Wait for lobbies ----
    const Clock::time_point drain_start = Clock::now();
    Clock::time_point last_placed = drain_start;
    Clock::time_point last_report = drain_start;
    for (int round = 0;; ++round) {
        std::vector<std::size_t> waiting;
        for (std::size_t i = 0; i < run.players.size(); ++i) {
            PlayerResult& player = run.players[i];
            if (!player.enqueued) {
                player.status = FinalStatus::kNeverEnqueued;
            } else if (player.status != FinalStatus::kInLobby) {
                waiting.push_back(i);  // lobbies are final; only re-check the rest
            }
        }
        if (waiting.empty()) {
            break;
        }
        std::atomic<std::size_t> placed{0};
        on_threads(std::min(thread_count, waiting.size()), [&](std::size_t t) {
            for (std::size_t w = t; w < waiting.size(); w += thread_count) {
                if (check(targets, waiting[w], round, config, run.players[waiting[w]])) {
                    ++placed;
                }
            }
        });
        const Clock::time_point now = Clock::now();
        if (placed > 0) {
            last_placed = now;
        }
        if (config.progress && now - last_report >= std::chrono::seconds(2)) {
            config.progress("waiting for lobbies: " + std::to_string(waiting.size() - placed) +
                            " players not yet placed");
            last_report = now;
        }
        if (waiting.size() == placed || now - last_placed >= config.drain_quiet ||
            now - drain_start >= config.drain_timeout) {
            break;
        }
        std::this_thread::sleep_for(config.poll_interval);
    }
    run.drain_duration = Clock::now() - drain_start;

    // ---- 3. Summarise ----
    for (const PlayerResult& player : run.players) {
        run.retries += static_cast<std::size_t>(std::max(0, player.attempts - 1));
        if (player.enqueued && !player.served_by.empty()) {
            ++run.served_by[player.served_by];
        }
        if (player.status == FinalStatus::kInLobby && player.enqueued_at && player.formed_at) {
            run.time_to_lobby.record(*player.formed_at - *player.enqueued_at);
        }
    }
    return run;
}

}  // namespace matchmesh::loadgen
