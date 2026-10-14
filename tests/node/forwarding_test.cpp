// End-to-end forwarding tests: three real nodes, each with its own engine,
// router and gRPC server, inside one test process.
//
// The nodes talk over Unix domain sockets (files like /tmp/.../node-a.sock)
// instead of TCP ports. That way every address is known before any server
// starts, so each node can be given the full peer list up front, just like
// in Docker Compose, and tests can never clash over port numbers.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::node {
namespace {

using namespace std::chrono_literals;

struct TestNode {
    std::unique_ptr<core::MatchmakingEngine> engine;
    std::unique_ptr<Router> router;
    std::unique_ptr<MatchmakingServiceImpl> service;
    std::unique_ptr<grpc::Server> server;
};

class ForwardingTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("mm-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(dir_);

        std::map<std::string, std::string> peers;
        for (const char* id : {"node-a", "node-b", "node-c"}) {
            peers[id] = "unix:" + (dir_ / (std::string(id) + ".sock")).string();
        }
        for (const auto& [id, address] : peers) {
            TestNode& node = nodes_[id];
            // No background workers: tests run match passes themselves.
            node.engine = std::make_unique<core::MatchmakingEngine>(
                id, core::EngineConfig{.worker_count = 0});
            node.router = std::make_unique<Router>(id, peers, 1s);
            node.service = std::make_unique<MatchmakingServiceImpl>(*node.engine, *node.router);

            grpc::ServerBuilder builder;
            builder.AddListeningPort(address, grpc::InsecureServerCredentials());
            builder.RegisterService(node.service.get());
            node.server = builder.BuildAndStart();
            ASSERT_NE(node.server, nullptr) << id;

            stubs_[id] = v1::MatchmakingService::NewStub(
                grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
        }
    }

    void TearDown() override {
        for (auto& [id, node] : nodes_) {
            if (node.server) {
                node.server->Shutdown(std::chrono::system_clock::now() + 1s);
            }
        }
        nodes_.clear();
        std::filesystem::remove_all(dir_);
    }

    // A queue (region "q<i>", ranked, bracket of rating 1300) owned by `owner`.
    std::string region_owned_by(const std::string& owner) const {
        const Router& router = *nodes_.at("node-a").router;
        for (int i = 0;; ++i) {
            const std::string region = "q" + std::to_string(i);
            if (router.owner_of(core::make_queue_key(region, "ranked", 1300)) == owner) {
                return region;
            }
        }
    }

    struct Result {
        grpc::Status status;
        std::string served_by;  // from the x-matchmesh-served-by response header
    };

    Result enqueue(const std::string& via, const std::string& player, const std::string& region,
                   v1::EnqueueResponse* response = nullptr) {
        v1::EnqueueRequest request;
        request.mutable_player()->set_player_id(player);
        request.mutable_player()->set_rating(1300);
        request.set_region(region);
        request.set_game_mode("ranked");
        v1::EnqueueResponse scratch;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        Result result{stubs_.at(via)->Enqueue(&context, request, response ? response : &scratch),
                      ""};
        result.served_by = served_by(context);
        return result;
    }

    v1::GetLobbyResponse get_lobby(const std::string& via, const std::string& player,
                                   const std::string& region, std::string* served = nullptr) {
        v1::GetLobbyRequest request;
        request.set_player_id(player);
        request.mutable_queue()->set_region(region);
        request.mutable_queue()->set_game_mode("ranked");
        request.mutable_queue()->set_skill_bracket(core::skill_bracket_for(1300));
        v1::GetLobbyResponse response;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        EXPECT_TRUE(stubs_.at(via)->GetLobby(&context, request, &response).ok());
        if (served != nullptr) {
            *served = served_by(context);
        }
        return response;
    }

    static std::string served_by(const grpc::ClientContext& context) {
        const auto& metadata = context.GetServerInitialMetadata();
        const auto it = metadata.find(grpc::string_ref(kServedByKey.data(), kServedByKey.size()));
        return it == metadata.end() ? "" : std::string(it->second.data(), it->second.size());
    }

    core::PlayerStatus status_on(const std::string& node, const std::string& player,
                                 const std::string& region) {
        return nodes_.at(node)
            .engine->lookup(core::make_queue_key(region, "ranked", 1300), player)
            .status;
    }

    std::filesystem::path dir_;
    std::map<std::string, TestNode> nodes_;
    std::map<std::string, std::unique_ptr<v1::MatchmakingService::Stub>> stubs_;
};

TEST_F(ForwardingTest, OwnQueueIsServedLocally) {
    const std::string region = region_owned_by("node-a");
    const Result result = enqueue("node-a", "alice", region);

    ASSERT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.served_by, "node-a");
    EXPECT_EQ(status_on("node-a", "alice", region), core::PlayerStatus::kQueued);
}

TEST_F(ForwardingTest, AnotherNodesQueueIsForwardedToItsOwner) {
    const std::string region = region_owned_by("node-c");
    v1::EnqueueResponse response;
    const Result result = enqueue("node-a", "alice", region, &response);

    ASSERT_TRUE(result.status.ok()) << result.status.error_message();
    EXPECT_EQ(result.served_by, "node-c");
    EXPECT_EQ(response.queue().region(), region);  // the owner's answer came back
    // Alice lives on the owner only.
    EXPECT_EQ(status_on("node-c", "alice", region), core::PlayerStatus::kQueued);
    EXPECT_EQ(status_on("node-a", "alice", region), core::PlayerStatus::kNotFound);
}

TEST_F(ForwardingTest, PlayersJoiningThroughDifferentNodesMeetInOneQueue) {
    // Four players reach the same queue through three different nodes. They
    // must all land on its owner, form one lobby there, and any node must be
    // able to report it.
    const std::string region = region_owned_by("node-b");
    ASSERT_TRUE(enqueue("node-a", "p1", region).status.ok());
    ASSERT_TRUE(enqueue("node-c", "p2", region).status.ok());
    ASSERT_TRUE(enqueue("node-b", "p3", region).status.ok());
    ASSERT_TRUE(enqueue("node-a", "p4", region).status.ok());

    EXPECT_EQ(nodes_.at("node-b").engine->queued_players(), 4u);
    EXPECT_EQ(nodes_.at("node-a").engine->queued_players(), 0u);
    EXPECT_EQ(nodes_.at("node-c").engine->queued_players(), 0u);

    ASSERT_EQ(nodes_.at("node-b").engine->run_match_pass(core::Clock::now()), 1u);

    std::string served;
    const v1::GetLobbyResponse lobby = get_lobby("node-c", "p1", region, &served);
    EXPECT_EQ(lobby.status(), v1::PLAYER_STATUS_IN_LOBBY);
    EXPECT_EQ(lobby.lobby().lobby_id(), "node-b-1");
    EXPECT_EQ(lobby.lobby().players_size(), 4);
    EXPECT_EQ(served, "node-b");
}

TEST_F(ForwardingTest, CancelIsForwardedToo) {
    const std::string region = region_owned_by("node-b");
    ASSERT_TRUE(enqueue("node-a", "alice", region).status.ok());

    v1::CancelRequest request;
    request.set_player_id("alice");
    request.mutable_queue()->set_region(region);
    request.mutable_queue()->set_game_mode("ranked");
    request.mutable_queue()->set_skill_bracket(core::skill_bracket_for(1300));
    v1::CancelResponse response;
    grpc::ClientContext context;
    ASSERT_TRUE(stubs_.at("node-c")->Cancel(&context, request, &response).ok());

    EXPECT_TRUE(response.cancelled());
    EXPECT_EQ(served_by(context), "node-b");
    EXPECT_EQ(status_on("node-b", "alice", region), core::PlayerStatus::kNotFound);
}

TEST_F(ForwardingTest, TheOwnersErrorsComeBackUnchanged) {
    const std::string region = region_owned_by("node-c");
    ASSERT_TRUE(enqueue("node-a", "alice", region).status.ok());

    EXPECT_EQ(enqueue("node-b", "alice", region).status.error_code(),
              grpc::StatusCode::ALREADY_EXISTS);
}

TEST_F(ForwardingTest, BadRequestsAreRejectedBeforeAnyForwarding) {
    const Result result = enqueue("node-a", "", region_owned_by("node-c"));
    EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(nodes_.at("node-c").engine->queued_players(), 0u);
}

TEST_F(ForwardingTest, AForwardedRequestIsNeverForwardedAgain) {
    // Pretend node-b already forwarded this to node-a, but node-a thinks
    // node-c owns it. Forwarding again could loop forever, so node-a refuses.
    const std::string region = region_owned_by("node-c");
    v1::EnqueueRequest request;
    request.mutable_player()->set_player_id("alice");
    request.mutable_player()->set_rating(1300);
    request.set_region(region);
    request.set_game_mode("ranked");
    v1::EnqueueResponse response;
    grpc::ClientContext context;
    context.AddMetadata(std::string(kForwardedByKey), "node-b");

    const grpc::Status status = stubs_.at("node-a")->Enqueue(&context, request, &response);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(nodes_.at("node-c").engine->queued_players(), 0u);
}

TEST_F(ForwardingTest, DisagreementDuringAViewChangeIsRetryable) {
    // The forwarder routed under epoch 7, but this node is still at epoch 0:
    // a new membership view is still spreading. That is temporary, so the
    // answer is UNAVAILABLE ("retry") rather than FAILED_PRECONDITION.
    const std::string region = region_owned_by("node-c");
    v1::EnqueueRequest request;
    request.mutable_player()->set_player_id("alice");
    request.mutable_player()->set_rating(1300);
    request.set_region(region);
    request.set_game_mode("ranked");
    v1::EnqueueResponse response;
    grpc::ClientContext context;
    context.AddMetadata(std::string(kForwardedByKey), "node-b");
    context.AddMetadata(std::string(kEpochKey), "7");

    const grpc::Status status = stubs_.at("node-a")->Enqueue(&context, request, &response);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE) << status.error_message();
    EXPECT_NE(status.error_message().find("epoch"), std::string::npos);
}

TEST(MisconfiguredClusterTest, DisagreeingNodesFailFastInsteadOfLooping) {
    // Two nodes given inconsistent peer lists, so each believes the *other*
    // owns a queue:
    //   node-a knows {node-a, node-b}       and thinks node-b owns it
    //   node-b knows {node-b, node-c}       and thinks node-c owns it,
    //   where "node-c" is (wrongly) node-a's address.
    // Without the forwarded-by marker the request would bounce a -> b -> a
    // -> b ... until it timed out. With it, node-b refuses to forward again.
    static std::atomic<int> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mm-loop-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    const std::string a_address = "unix:" + (dir / "a.sock").string();
    const std::string b_address = "unix:" + (dir / "b.sock").string();

    const Router a_router("node-a", {{"node-a", a_address}, {"node-b", b_address}}, 1s);
    const Router b_router("node-b", {{"node-b", b_address}, {"node-c", a_address}}, 1s);

    // A queue where the two rings disagree in exactly that way.
    core::QueueKey queue;
    std::string region;
    for (int i = 0;; ++i) {
        region = "q" + std::to_string(i);
        queue = core::make_queue_key(region, "ranked", 1300);
        if (a_router.owner_of(queue) == "node-b" && b_router.owner_of(queue) == "node-c") {
            break;
        }
    }

    core::MatchmakingEngine a_engine("node-a", core::EngineConfig{.worker_count = 0});
    core::MatchmakingEngine b_engine("node-b", core::EngineConfig{.worker_count = 0});
    MatchmakingServiceImpl a_service(a_engine, a_router);
    MatchmakingServiceImpl b_service(b_engine, b_router);
    grpc::ServerBuilder a_builder;
    a_builder.AddListeningPort(a_address, grpc::InsecureServerCredentials());
    a_builder.RegisterService(&a_service);
    const auto a_server = a_builder.BuildAndStart();
    grpc::ServerBuilder b_builder;
    b_builder.AddListeningPort(b_address, grpc::InsecureServerCredentials());
    b_builder.RegisterService(&b_service);
    const auto b_server = b_builder.BuildAndStart();
    ASSERT_NE(a_server, nullptr);
    ASSERT_NE(b_server, nullptr);

    const auto stub = v1::MatchmakingService::NewStub(
        grpc::CreateChannel(a_address, grpc::InsecureChannelCredentials()));
    v1::EnqueueRequest request;
    request.mutable_player()->set_player_id("alice");
    request.mutable_player()->set_rating(1300);
    request.set_region(region);
    request.set_game_mode("ranked");
    v1::EnqueueResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);

    const auto start = std::chrono::steady_clock::now();
    const grpc::Status status = stub->Enqueue(&context, request, &response);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION)
        << status.error_message();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s) << "bounced around instead of failing";
    EXPECT_EQ(a_engine.queued_players() + b_engine.queued_players(), 0u);

    a_server->Shutdown(std::chrono::system_clock::now());
    b_server->Shutdown(std::chrono::system_clock::now());
    std::filesystem::remove_all(dir);
}

TEST_F(ForwardingTest, AnUnreachableOwnerGivesUnavailableQuickly) {
    const std::string region = region_owned_by("node-c");
    nodes_.at("node-c").server->Shutdown(std::chrono::system_clock::now());

    const auto start = std::chrono::steady_clock::now();
    const Result result = enqueue("node-a", "alice", region);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result.status.error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_NE(result.status.error_message().find("node-c"), std::string::npos)
        << result.status.error_message();
    EXPECT_LT(elapsed, 3s) << "should give up within the 1 s forwarding timeout";
}

}  // namespace
}  // namespace matchmesh::node
