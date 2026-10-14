// End-to-end membership tests: real nodes (membership, router, engine,
// cluster manager and gRPC server each), heartbeating each other over Unix
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
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/router.hpp"
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
};

const Timing kFast{{.heartbeat_interval = 50ms,
                    .heartbeat_timeout = 40ms,
                    .check_interval = 10ms,
                    .log_changes = false},
                   {.suspect_after = 300ms}};
const Timing kReal{{.log_changes = false}, {}};  // the production defaults

// Everything one running node consists of. Members are declared in the order
// main() builds them, so they are destroyed in the safe reverse order.
struct RunningNode {
    std::unique_ptr<core::Membership> membership;
    std::unique_ptr<Router> router;
    std::unique_ptr<core::MatchmakingEngine> engine;
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

    void start(const std::string& id) {
        auto node = std::make_unique<RunningNode>();
        node->membership = std::make_unique<core::Membership>(id, ids_, core::Clock::now(),
                                                              timing_.membership);
        node->router = std::make_unique<Router>(id, peers_, 1s);
        node->engine = std::make_unique<core::MatchmakingEngine>(
            id, core::EngineConfig{.worker_count = 0});
        node->manager = std::make_unique<ClusterManager>(*node->membership, *node->router,
                                                         peers_.at(id), timing_.manager);
        node->cluster_service = std::make_unique<ClusterServiceImpl>(*node->manager);
        node->matchmaking_service =
            std::make_unique<MatchmakingServiceImpl>(*node->engine, *node->router);

        grpc::ServerBuilder builder;
        builder.AddListeningPort(peers_.at(id), grpc::InsecureServerCredentials());
        builder.RegisterService(node->cluster_service.get());
        builder.RegisterService(node->matchmaking_service.get());
        node->server = builder.BuildAndStart();
        ASSERT_NE(node->server, nullptr) << id;
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

    std::string owner_according_to(const std::string& id, const core::QueueKey& queue) const {
        return nodes_.at(id)->router->owner_of(queue);
    }

    grpc::Status enqueue_via(const std::string& id, const std::string& player,
                             const std::string& region) const {
        const auto stub = v1::MatchmakingService::NewStub(
            grpc::CreateChannel(peers_.at(id), grpc::InsecureChannelCredentials()));
        v1::EnqueueRequest request;
        request.mutable_player()->set_player_id(player);
        request.mutable_player()->set_rating(1300);
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

TEST(ClusterTest, AHealthyClusterStaysAtEpochZero) {
    TestCluster cluster(kThree, kFast);
    std::this_thread::sleep_for(600ms);  // two full suspicion windows
    EXPECT_TRUE(cluster.all_agree_on(kThree));
    EXPECT_EQ(cluster.view_of("node-b").epoch, 0u) << "nothing changed, so no new epoch";
}

TEST(ClusterTest, ACrashedNodeIsDroppedAndItsQueuesMove) {
    TestCluster cluster(kThree, kFast);
    const std::string region = region_owned_by("node-c");
    const core::QueueKey queue = core::make_queue_key(region, "ranked", 1300);
    ASSERT_EQ(cluster.owner_according_to("node-a", queue), "node-c");

    cluster.crash("node-c");

    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }));
    EXPECT_EQ(cluster.view_of("node-a").epoch, 1u);
    EXPECT_EQ(cluster.view_of("node-b").coordinator(), "node-a");

    // node-c's queue now belongs to a survivor, and works through any node.
    const std::string new_owner = cluster.owner_according_to("node-b", queue);
    EXPECT_NE(new_owner, "node-c");
    EXPECT_TRUE(cluster.enqueue_via("node-b", "alice", region).ok());
    EXPECT_EQ(cluster.engine_of(new_owner).queued_players(), 1u);
}

TEST(ClusterTest, WhenTheCoordinatorCrashesTheNextLowestTakesOver) {
    TestCluster cluster(kThree, kFast);
    cluster.crash("node-a");

    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-b", "node-c"}); }));
    const core::MembershipView view = cluster.view_of("node-c");
    EXPECT_EQ(view.coordinator(), "node-b");
    EXPECT_EQ(view.epoch, 1u);
}

TEST(ClusterTest, ARestartedNodeRejoinsWithANewerEpoch) {
    TestCluster cluster(kThree, kFast);
    cluster.crash("node-c");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }));

    cluster.start("node-c");  // a fresh process: it starts back at epoch 0

    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on(kThree); }));
    EXPECT_EQ(cluster.view_of("node-c").epoch, 2u) << "1 = node-c out, 2 = node-c back in";
}

TEST(ClusterTest, ARestartedCoordinatorTakesItsRoleBack) {
    TestCluster cluster(kThree, kFast);
    cluster.crash("node-a");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-b", "node-c"}); }));

    cluster.start("node-a");

    // node-a learns epoch 1 from the others, then (as the lowest node)
    // publishes epoch 2 with itself back in.
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on(kThree); }));
    const core::MembershipView view = cluster.view_of("node-b");
    EXPECT_EQ(view.coordinator(), "node-a");
    EXPECT_EQ(view.epoch, 2u);
}

TEST(ClusterTest, ANodeThatWasDownForAWhileIsReachableSoonAfterRestarting) {
    // While node-c is down, every connection to it keeps failing, and gRPC
    // waits longer and longer between reconnect attempts (by default 1 s,
    // growing 1.6x per failure, up to 2 minutes). When node-c returns and
    // takes its queues back, forwarded requests must reach it quickly, not
    // only once some long reconnect wait happens to run out.
    TestCluster cluster(kThree, kFast);
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

    // The first request after rejoining, through another node, lands on node-c.
    ASSERT_TRUE(eventually([&] { return cluster.enqueue_via("node-a", "alice", region).ok(); }));
    const auto took = std::chrono::steady_clock::now() - restarted_at;
    EXPECT_EQ(cluster.engine_of("node-c").queued_players(), 1u);
    EXPECT_LT(took, 1500ms) << "took "
                            << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                            << " ms";
}

TEST(ClusterTest, TopologyReportsTheAgreedView) {
    TestCluster cluster(kThree, kFast);
    cluster.crash("node-b");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-c"}); }));

    const v1::GetTopologyResponse topology = cluster.topology_from("node-c");
    EXPECT_EQ(topology.epoch(), 1u);
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

TEST(ClusterTest, WithRealTimingsACrashIsHandledInUnderTwoSeconds) {
    // The plan's target: heartbeats every 500 ms, suspicion after 1.5 s,
    // recovery (every survivor routing around the dead node) under 2 s.
    TestCluster cluster(kThree, kReal);
    std::this_thread::sleep_for(1s);  // let heartbeats settle

    const auto crashed_at = std::chrono::steady_clock::now();
    cluster.crash("node-c");
    ASSERT_TRUE(eventually([&] { return cluster.all_agree_on({"node-a", "node-b"}); }, 4s));
    const auto took = std::chrono::steady_clock::now() - crashed_at;

    EXPECT_LT(took, 2s) << "took "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                        << " ms";
}

}  // namespace
}  // namespace matchmesh::node
