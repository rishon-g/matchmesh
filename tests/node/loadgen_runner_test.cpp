// Tests for the load generator's runner against a real in-process node:
// players are sent, timed, and followed until they land in lobbies.

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/loadgen/runner.hpp"
#include "matchmesh/loadgen/workload.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/router.hpp"

namespace matchmesh::loadgen {
namespace {

using namespace std::chrono_literals;

// One standalone node with matchmaking workers, on a Unix socket.
class SoloNode {
public:
    SoloNode()
        : dir_(std::filesystem::temp_directory_path() /
               ("mm-loadgen-" + std::to_string(::getpid()))),
          router_("solo", {}),
          engine_("solo", core::EngineConfig{.worker_count = 2, .tick = 10ms}),
          service_(engine_, router_) {
        std::filesystem::create_directories(dir_);
        address_ = "unix:" + (dir_ / "solo.sock").string();
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
    }
    ~SoloNode() {
        server_->Shutdown(std::chrono::system_clock::now());
        std::filesystem::remove_all(dir_);
    }
    const std::string& address() const { return address_; }
    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path dir_;
    std::string address_;
    node::Router router_;
    core::MatchmakingEngine engine_;
    node::MatchmakingServiceImpl service_;
    std::unique_ptr<grpc::Server> server_;
};

RunConfig quick(std::vector<std::string> targets) {
    RunConfig config;
    config.targets = std::move(targets);
    config.threads = 8;
    config.drain_quiet = 1s;
    config.poll_interval = 50ms;
    return config;
}

TEST(LoadRunnerTest, EveryPlayerIsSentTimedAndFollowedIntoALobby) {
    const SoloNode node;
    // 400 players in one queue, with ratings close enough to match at once.
    const auto plan = make_workload({.players = 400,
                                     .arrivals_per_second = 2'000,
                                     .rating_mean = 1'750,
                                     .rating_stddev = 10,
                                     .regions = {{"eu", 1}},
                                     .modes = {{"ranked", 1}}});

    const RunResult run = run_load(plan, quick({node.address()}));

    EXPECT_EQ(run.enqueue_latency.count(), 400u);
    EXPECT_EQ(run.count(FinalStatus::kNeverEnqueued), 0u);
    EXPECT_EQ(run.count(FinalStatus::kNotFound), 0u);
    EXPECT_EQ(run.count(FinalStatus::kUnknown), 0u);
    EXPECT_EQ(run.count(FinalStatus::kInLobby) + run.count(FinalStatus::kQueued), 400u);
    EXPECT_LT(run.count(FinalStatus::kQueued), 4u) << "only a leftover smaller than a lobby";
    EXPECT_EQ(run.lobbies() * 4, run.count(FinalStatus::kInLobby));
    EXPECT_EQ(run.time_to_lobby.count(), run.count(FinalStatus::kInLobby));
    EXPECT_EQ(run.served_by.at("solo"), 400u);
    EXPECT_EQ(run.retries, 0u);

    // Every lobby a player reports has four players, including that player.
    for (const PlayerResult& player : run.players) {
        if (player.status == FinalStatus::kInLobby) {
            ASSERT_EQ(player.lobby_players.size(), 4u);
            EXPECT_TRUE(std::ranges::find(player.lobby_players, player.player.id) !=
                        player.lobby_players.end());
        }
    }
}

TEST(LoadRunnerTest, LatencyCountsTheTimeAPlayerWaitedToBeSent) {
    // One client thread and 200 players all due at the same instant: player k
    // can only be sent after the k before it, and that wait is part of what
    // they experienced. Timing from the moment of sending instead (the
    // coordinated-omission mistake) would make every player look equally fast.
    const SoloNode node;
    RunConfig config = quick({node.address()});
    config.threads = 1;
    const auto plan = make_workload({.players = 200, .arrivals_per_second = 0});

    const RunResult run = run_load(plan, config);

    ASSERT_EQ(run.enqueue_latency.count(), 200u);
    // Waits grow steadily down the line: the middle player waited for ~100
    // others, the first few for almost none.
    EXPECT_GE(run.enqueue_latency.percentile(50), run.enqueue_latency.percentile(1) * 10);
}

TEST(LoadRunnerTest, ADeadTargetIsRetriedOnTheNextOne) {
    const SoloNode node;
    const std::string dead = "unix:" + (node.dir() / "nobody.sock").string();
    const auto plan = make_workload({.players = 50, .arrivals_per_second = 0});

    // Half the players are sent to the dead address first.
    const RunResult run = run_load(plan, quick({dead, node.address()}));

    EXPECT_EQ(run.count(FinalStatus::kNeverEnqueued), 0u);
    EXPECT_GE(run.retries, 25u);
    EXPECT_EQ(run.enqueue_latency.count(), 50u);
}

TEST(LoadRunnerTest, PlayersThatCannotBeSentAreReportedNotTimed) {
    const SoloNode node;
    const std::string dead = "unix:" + (node.dir() / "nobody.sock").string();
    RunConfig config = quick({dead});
    config.give_up_after = 300ms;
    const auto plan = make_workload({.players = 10, .arrivals_per_second = 0});

    const RunResult run = run_load(plan, config);

    EXPECT_EQ(run.count(FinalStatus::kNeverEnqueued), 10u);
    EXPECT_EQ(run.enqueue_latency.count(), 0u);
    EXPECT_FALSE(run.players.front().last_error.empty());
}

}  // namespace
}  // namespace matchmesh::loadgen
