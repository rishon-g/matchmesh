// End-to-end membership and replication tests: real nodes (membership,
// router, replication, engine, shard manager, cluster manager and gRPC
// server each, wired exactly as in main), heartbeating each other over Unix
// sockets inside one test process. Nodes can be crashed and restarted.
//
// Most tests use shortened timings (50 ms heartbeats, 300 ms suspicion) so
// they run quickly; one uses the real defaults to check the under-2-second
// recovery target.

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/node/cluster_manager.hpp"
#include "matchmesh/node/cluster_service.hpp"
#include "matchmesh/node/convert.hpp"
#include "matchmesh/node/identity.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/replication_client.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/node/shard_manager.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::node {
namespace {

using namespace std::chrono_literals;

template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

struct Timing {
    ClusterManagerConfig manager;
    core::MembershipConfig membership;
    ShardManagerConfig shards;
    std::chrono::milliseconds replicate_timeout;
};

const Timing kFast{{.heartbeat_interval = 50ms,
                    .heartbeat_timeout = 40ms,
                    .check_interval = 10ms,
                    .log_changes = false},
                   {.suspect_after = 300ms},
                   {.retry_interval = 20ms, .log_changes = false},
                   200ms};
// The production defaults.
const Timing kReal{{.log_changes = false}, {}, {.log_changes = false}, 500ms};

// Everything one running node consists of. Members are declared in the order
// main() builds them, so they are destroyed in the safe reverse order.
struct RunningNode {
    std::unique_ptr<core::Membership> membership;
    std::unique_ptr<Router> router;
    std::unique_ptr<ReplicationClient> replication;
    std::unique_ptr<core::MatchmakingEngine> engine;
    std::unique_ptr<ShardManager> shards;
    std::unique_ptr<ClusterManager> manager;
    std::unique_ptr<ClusterServiceImpl> cluster_service;
    std::unique_ptr<MatchmakingServiceImpl> matchmaking_service;
    std::unique_ptr<grpc::Server> server;
};

class TestCluster {
public:
    TestCluster(std::vector<std::string> ids, Timing timing) : ids_(std::move(ids)), timing_(timing) {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("mm-cl-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(dir_);
        for (const std::string& id : ids_) {
            peers_[id] = "unix:" + (dir_ / (id + ".sock")).string();
        }
        for (const std::string& id : ids_) {
            start(id);
        }
    }

    ~TestCluster() {
        for (const std::string& id : ids_) {
            crash(id);
        }
        std::filesystem::remove_all(dir_);
    }

    // Starts `id` as a brand-new process: empty memory, new incarnation.
    // Its engine runs no workers; tests call run_match_pass themselves.
    void start(const std::string& id) {
        static std::atomic<std::uint64_t> next_incarnation{1000};
        const std::uint64_t incarnation = next_incarnation++;
        auto node = std::make_unique<RunningNode>();
        node->membership = std::make_unique<core::Membership>(
            id, ids_, core::Clock::now(), timing_.membership, incarnation);
        node->router = std::make_unique<Router>(id, peers_, 1s);
        node->replication =
            std::make_unique<ReplicationClient>(*node->router, timing_.replicate_timeout);
        node->engine = std::make_unique<core::MatchmakingEngine>(
            lobby_id_prefix(id, incarnation), core::EngineConfig{.worker_count = 0}, core::Clock::now,
            node->replication.get());
        node->shards = std::make_unique<ShardManager>(*node->engine, *node->router,
                                                      *node->replication, incarnation,
                                                      timing_.shards);
        node->manager = std::make_unique<ClusterManager>(*node->membership, *node->router,
                                                         peers_.at(id), timing_.manager,
                                                         node->shards.get());
        node->cluster_service =
            std::make_unique<ClusterServiceImpl>(*node->manager, node->shards.get());
        node->matchmaking_service = std::make_unique<MatchmakingServiceImpl>(
            *node->engine, *node->router, node->shards.get());

        grpc::ServerBuilder builder;
        builder.AddListeningPort(peers_.at(id), grpc::InsecureServerCredentials());
        builder.RegisterService(node->cluster_service.get());
        builder.RegisterService(node->matchmaking_service.get());
        node->server = builder.BuildAndStart();
        ASSERT_NE(node->server, nullptr) << id;
        node->shards->start();
        node->manager->start();
        nodes_[id] = std::move(node);
    }

    // Stops a node abruptly: it simply stops answering, like a crash.
    void crash(const std::string& id) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) {
            return;
        }
        it->second->server->Shutdown(std::chrono::system_clock::now());
        it->second->manager->stop();
        it->second->shards->stop();
        nodes_.erase(it);
        std::filesystem::remove(dir_ / (id + ".sock"));
    }

    core::MembershipView view_of(const std::string& id) const {
        return nodes_.at(id)->manager->view();
    }

    // True once every running node holds the same view with exactly `members`.
    bool all_agree_on(const std::vector<std::string>& members) const {
        std::optional<core::MembershipView> first;
        for (const auto& [id, node] : nodes_) {
            const core::MembershipView view = node->manager->view();
            if (view.members != members || (first && view != *first) ||
                node->router->epoch() != view.epoch) {
                return false;
            }
            first = view;
        }
        return true;
    }

    // True once every running node agrees on a view of exactly the running
    // nodes and has finished handing queues over for it, so every queue is
    // being served by its owner.
    bool settled() const {
        std::vector<std::string> running;
        for (const auto& [id, node] : nodes_) {
            running.push_back(id);
        }
        if (!all_agree_on(running)) {
            return false;
        }
        return std::ranges::all_of(nodes_, [](const auto& entry) {
            return entry.second->shards->settled(entry.second->router->view());
        });
    }

    std::string owner_according_to(const std::string& id, const core::QueueKey& queue) const {
        return nodes_.at(id)->router->owner_of(queue);
    }

    Placement placement_according_to(const std::string& id, const core::QueueKey& queue) const {
        return nodes_.at(id)->router->placement(queue);
    }

    struct LobbyAnswer {
        grpc::Status status;
        v1::GetLobbyResponse response;
        std::string served_by;
    };

    LobbyAnswer get_lobby_via(const std::string& id, const std::string& player,
                              const std::string& region, int rating = 1300) const {
        const auto stub = v1::MatchmakingService::NewStub(
            grpc::CreateChannel(peers_.at(id), grpc::InsecureChannelCredentials()));
        v1::GetLobbyRequest request;
        request.set_player_id(player);
        to_proto(core::make_queue_key(region, "ranked", rating), request.mutable_queue());
        LobbyAnswer answer;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        answer.status = stub->GetLobby(&context, request, &answer.response);
        const auto& metadata = context.GetServerInitialMetadata();
        if (const auto it = metadata.find(kServedByKey.data()); it != metadata.end()) {
            answer.served_by = std::string(it->second.data(), it->second.size());
        }
        return answer;
    }

    // Retries (as a real client would) until the player's status can be read.
    std::optional<LobbyAnswer> lobby_of(const std::string& via, const std::string& player,
                                        const std::string& region, int rating = 1300) const {
        std::optional<LobbyAnswer> answer;
        if (!eventually([&] {
                answer = get_lobby_via(via, player, region, rating);
                return answer->status.ok();
            })) {
            return std::nullopt;
        }
        return answer;
    }

    v1::ReplicateResponse replicate_to(const std::string& id,
                                       const v1::ReplicateRequest& request) const {
        const auto stub = v1::ClusterService::NewStub(
            grpc::CreateChannel(peers_.at(id), grpc::InsecureChannelCredentials()));
        v1::ReplicateResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        EXPECT_TRUE(stub->Replicate(&context, request, &response).ok());
        return response;
    }

    bool running(const std::string& id) const { return nodes_.contains(id); }

    grpc::Status enqueue_via(const std::string& id, const std::string& player,
                             const std::string& region, int rating = 1300,
                             const std::string& request_id = {}) const {
        const auto stub = v1::MatchmakingService::NewStub(
            grpc::CreateChannel(peers_.at(id), grpc::InsecureChannelCredentials()));
        v1::EnqueueRequest request;
        request.mutable_player()->set_player_id(player);
        request.mutable_player()->set_rating(rating);
        request.set_request_id(request_id);
        request.set_region(region);
        request.set_game_mode("ranked");
        v1::EnqueueResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        return stub->Enqueue(&context, request, &response);
    }

    v1::GetTopologyResponse topology_from(const std::string& id) const {
        const auto stub = v1::ClusterService::NewStub(
            grpc::CreateChannel(peers_.at(id), grpc::InsecureChannelCredentials()));
        v1::GetTopologyResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        EXPECT_TRUE(stub->GetTopology(&context, v1::GetTopologyRequest{}, &response).ok());
        return response;
    }

    core::MatchmakingEngine& engine_of(const std::string& id) { return *nodes_.at(id)->engine; }

private:
    std::vector<std::string> ids_;
    Timing timing_;
    std::filesystem::path dir_;
    std::map<std::string, std::string> peers_;
    std::map<std::string, std::unique_ptr<RunningNode>> nodes_;
};

const std::vector<std::string> kThree = {"node-a", "node-b", "node-c"};

// A region whose ranked/1300 queue `owner` holds under epoch 0 (all nodes).
std::string region_owned_by(const std::string& owner) {
    std::map<std::string, std::string> peers;
    for (const std::string& id : kThree) {
        peers[id] = "unused";
    }
    const Router router("node-a", peers);
    for (int i = 0;; ++i) {
        const std::string region = "q" + std::to_string(i);
        if (router.owner_of(core::make_queue_key(region, "ranked", 1300)) == owner) {
            return region;
        }
    }
}

TEST(ClusterTest, AHealthyClusterSettlesOnOneEpochAndStaysThere) {
    TestCluster cluster(kThree, kFast);
    // With real incarnations, epoch 0 records none, so the coordinator
    // publishes epoch 1 as soon as it has heard from everyone.
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const core::MembershipView view = cluster.view_of("node-b");
    EXPECT_EQ(view.epoch, 1u);
    EXPECT_EQ(view.incarnations.size(), 3u);

    std::this_thread::sleep_for(600ms);  // two full suspicion windows
    EXPECT_EQ(cluster.view_of("node-b"), view) << "nothing changed, so no new epoch";
}

TEST(ClusterTest, ACrashedNodeIsDroppedAndItsQueuesMove) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::uint64_t before = cluster.view_of("node-a").epoch;
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    ASSERT_EQ(cluster.owner_according_to("node-a", queue), "node-c");

    cluster.crash("node-c");

    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }));
    EXPECT_EQ(cluster.view_of("node-a").epoch, before + 1);
    EXPECT_EQ(cluster.view_of("node-b").coordinator(), "node-a");

    // node-c's queue now belongs to a survivor, and works through any node.
    const std::string new_owner = cluster.owner_according_to("node-b", queue);
    EXPECT_NE(new_owner, "node-c");
    EXPECT_TRUE(eventually([&] { return cluster.enqueue_via("node-b", "alice", region).ok(); }));
    EXPECT_EQ(cluster.engine_of(new_owner).queued_players(), 1u);
}

TEST(ClusterTest, WhenTheCoordinatorCrashesTheNextLowestTakesOver) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::uint64_t before = cluster.view_of("node-c").epoch;
    cluster.crash("node-a");

    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-b", "node-c"}); }));
    const core::MembershipView view = cluster.view_of("node-c");
    EXPECT_EQ(view.coordinator(), "node-b");
    EXPECT_EQ(view.epoch, before + 1);
}

TEST(ClusterTest, ARestartedNodeRejoinsWithANewerEpoch) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::uint64_t before = cluster.view_of("node-a").epoch;
    cluster.crash("node-c");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }));

    cluster.start("node-c");  // a fresh process: it starts back at epoch 0

    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    EXPECT_EQ(cluster.view_of("node-c").epoch, before + 2) << "+1 node-c out, +2 back in";
}

TEST(ClusterTest, ARestartedCoordinatorTakesItsRoleBack) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    cluster.crash("node-a");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-b", "node-c"}); }));
    const std::uint64_t without_a = cluster.view_of("node-b").epoch;

    cluster.start("node-a");

    // node-a learns the current epoch from the others, then (as the lowest
    // node) publishes the next one with itself back in.
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const core::MembershipView view = cluster.view_of("node-b");
    EXPECT_EQ(view.coordinator(), "node-a");
    EXPECT_GT(view.epoch, without_a);
}

TEST(ClusterTest, ANodeThatRestartsBeforeAnyoneNoticesIsStillGivenANewEpoch) {
    // Restarting faster than the 1.5 s failure detector: nobody ever sees
    // node-c silent, but the new process has lost everything it held. Its
    // new incarnation makes the coordinator publish a new epoch anyway.
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::uint64_t before = cluster.view_of("node-a").epoch;
    const std::string region = region_owned_by("node-c");
    ASSERT_TRUE(cluster.enqueue_via("node-a", "alice", region).ok());

    cluster.crash("node-c");
    cluster.start("node-c");

    // The very first answer about alice, asked straight away, must already
    // be right: the new node-c may not serve its queue until it has it back.
    const auto answer = cluster.lobby_of("node-a", "alice", region);
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->response.status(), v1::PLAYER_STATUS_QUEUED);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    EXPECT_GT(cluster.view_of("node-a").epoch, before);
}

TEST(ClusterTest, ANodeThatWasDownForAWhileIsReachableSoonAfterRestarting) {
    // While node-c is down, every connection to it keeps failing, and gRPC
    // waits longer and longer between reconnect attempts (by default 1 s,
    // growing 1.6x per failure, up to 2 minutes). When node-c returns and
    // takes its queues back, forwarded requests must reach it quickly, not
    // only once some long reconnect wait happens to run out.
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    // Real traffic first, so node-a holds a live connection to node-c.
    ASSERT_TRUE(eventually([&] { return cluster.enqueue_via("node-a", "before", region).ok(); }));

    cluster.crash("node-c");
    // Requests keep arriving while node-c is down; until the view changes,
    // node-a keeps trying (and failing) to forward them to node-c.
    for (int i = 0; i < 5; ++i) {
        (void)cluster.enqueue_via("node-a", "during-" + std::to_string(i), region);
    }
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }));
    std::this_thread::sleep_for(8s);  // node-c stays down for a while

    cluster.start("node-c");
    const auto restarted_at = std::chrono::steady_clock::now();

    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on(kThree); }));

    // The first request after rejoining, through another node, lands on
    // node-c, which got its queue back with everyone still in it.
    ASSERT_TRUE(eventually([&] { return cluster.enqueue_via("node-a", "alice", region).ok(); }));
    const auto took = std::chrono::steady_clock::now() - restarted_at;
    // "before" (kept by the backup the whole time node-c was down) and
    // alice. The "during" calls were refused retryably, so never added.
    EXPECT_EQ(cluster.engine_of("node-c").queued_players(), 2u)
        << "the queue came back without everyone in it";
    EXPECT_LT(took, 1500ms) << "took "
                            << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                            << " ms";
}

TEST(ClusterTest, TopologyReportsTheAgreedView) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::uint64_t before = cluster.view_of("node-a").epoch;
    cluster.crash("node-b");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-c"}); }));

    const v1::GetTopologyResponse topology = cluster.topology_from("node-c");
    EXPECT_EQ(topology.epoch(), before + 1);
    EXPECT_EQ(topology.coordinator_node_id(), "node-a");
    ASSERT_EQ(topology.nodes_size(), 3);
    std::map<std::string, bool> alive;
    for (const auto& node : topology.nodes()) {
        alive[node.node_id()] = node.alive();
    }
    EXPECT_TRUE(alive["node-a"]);
    EXPECT_FALSE(alive["node-b"]);
    EXPECT_TRUE(alive["node-c"]);
}

// ---- Replication and failover ----

TEST(ClusterTest, EveryChangeReachesTheBackupBeforeThePlayerIsAnswered) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    const Placement place = cluster.placement_according_to("node-a", queue);
    ASSERT_EQ(place.primary, "node-c");
    ASSERT_TRUE(place.backup.has_value());

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(cluster.enqueue_via("node-a", "p" + std::to_string(i), region).ok());
        // The moment the call returns, the backup already has the player.
        const auto copy = cluster.engine_of(*place.backup).shard_info(queue);
        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->role, core::ShardRole::kReplica);
        EXPECT_EQ(copy->queued, static_cast<std::size_t>(i + 1));
    }
    const std::string other = *place.backup == "node-a" ? "node-b" : "node-a";
    EXPECT_FALSE(cluster.engine_of(other).shard_info(queue).has_value())
        << "only the primary and its backup hold the queue";
}

TEST(ClusterTest, PlayersSurviveTheirPrimaryCrashing) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(cluster.enqueue_via("node-a", "p" + std::to_string(i), region).ok());
    }

    cluster.crash("node-c");

    for (int i = 0; i < 6; ++i) {
        const auto answer = cluster.lobby_of("node-a", "p" + std::to_string(i), region);
        ASSERT_TRUE(answer.has_value());
        EXPECT_EQ(answer->response.status(), v1::PLAYER_STATUS_QUEUED) << "p" << i << " was lost";
        EXPECT_NE(answer->served_by, "node-c");
    }
}

TEST(ClusterTest, ALobbyFormedBeforeACrashSurvivesItWhole) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const std::vector<std::string> players = {"ann", "ben", "cat", "dan"};
    for (std::size_t i = 0; i < players.size(); ++i) {
        ASSERT_TRUE(
            cluster.enqueue_via("node-a", players[i], region, 1300 + static_cast<int>(i)).ok());
    }
    ASSERT_EQ(cluster.engine_of("node-c").run_match_pass(core::Clock::now()), 1u);
    const auto formed = cluster.lobby_of("node-a", "ann", region);
    ASSERT_TRUE(formed.has_value());
    ASSERT_EQ(formed->response.status(), v1::PLAYER_STATUS_IN_LOBBY);
    const std::string lobby_id = formed->response.lobby().lobby_id();

    cluster.crash("node-c");

    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string new_owner = cluster.owner_according_to("node-a", queue);
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto answer =
            cluster.lobby_of("node-b", players[i], region, 1300 + static_cast<int>(i));
        ASSERT_TRUE(answer.has_value());
        EXPECT_EQ(answer->response.status(), v1::PLAYER_STATUS_IN_LOBBY) << players[i];
        EXPECT_EQ(answer->response.lobby().lobby_id(), lobby_id) << players[i];
    }
    // And nobody is still queued to be matched a second time.
    EXPECT_EQ(cluster.engine_of(new_owner).shard_info(queue)->queued, 0u);
    EXPECT_EQ(cluster.engine_of(new_owner).run_match_pass(core::Clock::now()), 0u);
}

TEST(ClusterTest, ANewBackupGetsACopySoASecondCrashLosesNoOneEither) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(
            cluster.enqueue_via("node-a", "p" + std::to_string(i), region, 1000 + 100 * i).ok());
    }
    // Ratings 1000-1300 are one bracket but too far apart to match yet.
    const std::string backup = *cluster.placement_according_to("node-a", queue).backup;
    const std::string last = backup == "node-a" ? "node-b" : "node-a";

    // The backup dies first. node-c keeps the queue; the remaining node
    // becomes its backup and is sent a full copy, without any new traffic.
    cluster.crash(backup);
    ASSERT_TRUE(eventually([&] {
        const auto copy = cluster.engine_of(last).shard_info(queue);
        return copy && copy->role == core::ShardRole::kReplica && copy->queued == 4;
    }));

    // Then the primary dies too. The last node standing has everyone.
    cluster.crash("node-c");
    for (int i = 0; i < 4; ++i) {
        const auto answer = cluster.lobby_of(last, "p" + std::to_string(i), region, 1000 + 100 * i);
        ASSERT_TRUE(answer.has_value());
        EXPECT_EQ(answer->response.status(), v1::PLAYER_STATUS_QUEUED) << "p" << i;
    }
}

TEST(ClusterTest, ARestartedNodeGetsItsQueuesBackWithEveryoneInThem) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(cluster.enqueue_via("node-a", "early" + std::to_string(i), region).ok());
    }
    cluster.crash("node-c");
    // While node-c is gone, its backup serves the queue and takes more players.
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(eventually(
            [&] { return cluster.enqueue_via("node-a", "late" + std::to_string(i), region).ok(); }));
    }

    cluster.start("node-c");  // empty memory

    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    for (const char* player : {"early0", "early1", "early2", "late0", "late1"}) {
        const auto answer = cluster.lobby_of("node-a", player, region);
        ASSERT_TRUE(answer.has_value());
        EXPECT_EQ(answer->response.status(), v1::PLAYER_STATUS_QUEUED) << player;
        EXPECT_EQ(answer->served_by, "node-c") << player;
    }
    const auto info = cluster.engine_of("node-c").shard_info(queue);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->role, core::ShardRole::kPrimary);
    EXPECT_EQ(info->queued, 5u);
}

TEST(ClusterTest, ARetryAfterALostReplyNeverJoinsAPlayerTwice) {
    // Found by scripts/chaos.sh: the node relaying an enqueue died after the
    // owner had added the player, so the client never heard "you're in" and
    // retried, by then the player was in a lobby and the retry queued them
    // again. Here the reply is "lost" by simply not looking at it.
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    for (int i = 0; i < 4; ++i) {
        const std::string player = "p" + std::to_string(i);
        ASSERT_TRUE(cluster.enqueue_via("node-a", player, region, 1300, "join-" + player).ok());
    }
    ASSERT_EQ(cluster.engine_of("node-c").run_match_pass(core::Clock::now()), 1u);
    const std::string lobby = cluster.lobby_of("node-a", "p0", region)->response.lobby().lobby_id();

    // The retry, through a different node: accepted as already done.
    EXPECT_TRUE(cluster.enqueue_via("node-b", "p0", region, 1300, "join-p0").ok());
    EXPECT_EQ(cluster.engine_of("node-c").shard_info(queue)->queued, 0u);

    // Even when it only arrives after the owner died and its backup took over.
    cluster.crash("node-c");
    EXPECT_TRUE(eventually(
        [&] { return cluster.enqueue_via("node-a", "p1", region, 1300, "join-p1").ok(); }));
    const std::string new_owner = cluster.owner_according_to("node-a", queue);
    EXPECT_EQ(cluster.engine_of(new_owner).shard_info(queue)->queued, 0u);
    for (const char* player : {"p0", "p1", "p2", "p3"}) {
        EXPECT_EQ(cluster.lobby_of("node-a", player, region)->response.lobby().lobby_id(), lobby)
            << player;
    }
}

TEST(ClusterTest, ARestartedNodeNeverReusesALobbyId) {
    // Found by scripts/chaos.sh: a restarted node's lobby counter starts
    // again at 1, so without the run's incarnation in the ID it handed out
    // "node-c-1" again, for a different lobby.
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const auto form_lobby = [&](const std::string& prefix) -> std::string {
        for (int i = 0; i < 4; ++i) {
            const auto status = [&] {
                return cluster.enqueue_via("node-a", prefix + std::to_string(i), region).ok();
            };
            EXPECT_TRUE(eventually(status));
        }
        EXPECT_EQ(cluster.engine_of("node-c").run_match_pass(core::Clock::now()), 1u);
        const auto answer = cluster.lobby_of("node-a", prefix + "0", region);
        return answer ? answer->response.lobby().lobby_id() : "";
    };

    const std::string first = form_lobby("before");
    cluster.crash("node-c");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }));
    cluster.start("node-c");
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string second = form_lobby("after");

    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
    EXPECT_EQ(first.rfind("node-c-", 0), 0u);
    EXPECT_EQ(second.rfind("node-c-", 0), 0u);
    // And the first lobby, carried through the restart, still says the same.
    EXPECT_EQ(cluster.lobby_of("node-a", "before0", region)->response.lobby().lobby_id(), first);
}

TEST(ClusterTest, ReplicationFromAnOlderEpochOrTheWrongNodeIsRejected) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    ASSERT_TRUE(cluster.enqueue_via("node-a", "alice", region).ok());
    const std::string backup = *cluster.placement_according_to("node-a", queue).backup;
    const std::uint64_t epoch = cluster.view_of("node-a").epoch;

    // A change that would wipe alice, as if from node-c at an earlier epoch.
    v1::ReplicateRequest stale;
    stale.set_sender_node_id("node-c");
    stale.set_epoch(epoch - 1);
    to_proto(core::ShardState{queue, 99, {}, {}, {}}, &stale);
    const v1::ReplicateResponse refused = cluster.replicate_to(backup, stale);
    EXPECT_FALSE(refused.accepted());
    EXPECT_EQ(refused.epoch(), epoch) << "tells the sender which epoch is current";

    // The right epoch, but not from the queue's primary.
    v1::ReplicateRequest impostor = stale;
    impostor.set_epoch(epoch);
    impostor.set_sender_node_id(backup == "node-a" ? "node-b" : "node-a");
    EXPECT_FALSE(cluster.replicate_to(backup, impostor).accepted());

    EXPECT_EQ(cluster.engine_of(backup).shard_info(queue)->queued, 1u) << "alice is untouched";
}

TEST(ClusterTest, AChangeTheBackupCannotConfirmIsRefusedRetryably) {
    TestCluster cluster(kThree, kFast);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }));
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    const std::string backup = *cluster.placement_according_to("node-c", queue).backup;
    const std::string via = backup == "node-a" ? "node-b" : "node-a";

    cluster.crash(backup);
    // Before the cluster notices, node-c can't copy the change, so it
    // doesn't make it: the player is told to retry rather than told "done".
    const grpc::Status first = cluster.enqueue_via(via, "alice", region);
    EXPECT_EQ(first.error_code(), grpc::StatusCode::UNAVAILABLE) << first.error_message();
    EXPECT_EQ(cluster.engine_of("node-c").shard_info(queue).value_or(core::ShardInfo{}).queued,
              0u);

    // Once the view changes, the retry succeeds (with a new backup).
    EXPECT_TRUE(eventually([&] { return cluster.enqueue_via(via, "alice", region).ok(); }));
}

// A peer that only answers heartbeats and never sends any (as when the
// network lets traffic through in one direction only). It reports a newer view.
class AnswerOnlyPeer final : public v1::ClusterService::Service {
public:
    grpc::Status Heartbeat(grpc::ServerContext* /*context*/, const v1::HeartbeatRequest* /*request*/,
                           v1::HeartbeatResponse* response) override {
        response->set_node_id("node-b");
        response->set_epoch(5);
        response->add_members("node-a");
        response->add_members("node-b");
        return grpc::Status::OK;
    }
};

TEST(ClusterTest, ViewsAlsoSpreadThroughHeartbeatReplies) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mm-reply-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::string a_address = "unix:" + (dir / "a.sock").string();
    const std::string b_address = "unix:" + (dir / "b.sock").string();

    AnswerOnlyPeer peer;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(b_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&peer);
    const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    core::Membership membership("node-a", {"node-a", "node-b"}, core::Clock::now(),
                                kFast.membership);
    Router router("node-a", {{"node-a", a_address}, {"node-b", b_address}}, 1s);
    ClusterManager manager(membership, router, a_address, kFast.manager);
    manager.start();

    // node-b never calls us, so the only way to learn its view is its replies.
    EXPECT_TRUE(eventually([&] { return manager.view().epoch == 5; }));
    EXPECT_EQ(router.epoch(), 5U);

    manager.stop();
    server->Shutdown(std::chrono::system_clock::now());
    std::filesystem::remove_all(dir);
}

TEST(ClusterTest, WithRealTimingsAFailoverLosesNoOneAndTakesUnderTwoSeconds) {
    // The plan's target: heartbeats every 500 ms, suspicion after 1.5 s,
    // recovery under 2 s. Measured from the crash to the queue answering
    // again through another node, with every player still in it.
    TestCluster cluster(kThree, kReal);
    ASSERT_TRUE(eventually([&] { return cluster.settled(); }, 4s));
    const std::string region = region_owned_by("node-c");
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(cluster.enqueue_via("node-a", "p" + std::to_string(i), region).ok());
    }

    const auto crashed_at = std::chrono::steady_clock::now();
    cluster.crash("node-c");
    ASSERT_TRUE(eventually(
        [&] { return cluster.get_lobby_via("node-a", "p0", region).status.ok(); }, 4s));
    const auto took = std::chrono::steady_clock::now() - crashed_at;

    EXPECT_LT(took, 2s) << "took "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                        << " ms";
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(cluster.get_lobby_via("node-a", "p" + std::to_string(i), region)
                      .response.status(),
                  v1::PLAYER_STATUS_QUEUED)
            << "p" << i;
    }
}

}  // namespace
}  // namespace matchmesh::node
