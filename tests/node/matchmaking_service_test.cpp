// End-to-end tests for MatchmakingService: a real gRPC server on a local
// port, called through a real client stub, backed by a real engine.

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace matchmesh::node {
namespace {

using namespace std::chrono_literals;

class MatchmakingServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        int port = 0;
        // Port 0 = "pick any free port", so tests never clash with each other
        // or with a node you're running.
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        ASSERT_GT(port, 0);

        stub_ = v1::MatchmakingService::NewStub(grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }

    void TearDown() override { server_->Shutdown(); }

    grpc::Status enqueue(const std::string& id, int rating, v1::EnqueueResponse* response,
                         const std::string& region = "us-west",
                         const std::string& mode = "ranked") {
        v1::EnqueueRequest request;
        request.mutable_player()->set_player_id(id);
        request.mutable_player()->set_rating(rating);
        request.set_region(region);
        request.set_game_mode(mode);
        grpc::ClientContext context;
        return stub_->Enqueue(&context, request, response);
    }

    v1::GetLobbyResponse get_lobby(const std::string& id, const v1::QueueKey& queue) {
        v1::GetLobbyRequest request;
        request.set_player_id(id);
        *request.mutable_queue() = queue;
        v1::GetLobbyResponse response;
        grpc::ClientContext context;
        EXPECT_TRUE(stub_->GetLobby(&context, request, &response).ok());
        return response;
    }

    // Workers tick fast so lobbies form quickly in tests.
    core::MatchmakingEngine engine_{"node-t",
                                    core::EngineConfig{.worker_count = 2, .tick = 10ms}};
    Router router_{"node-t", {}};  // a one-node cluster: everything is local
    MatchmakingServiceImpl service_{engine_, router_};
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<v1::MatchmakingService::Stub> stub_;
};

TEST_F(MatchmakingServiceTest, EnqueueReturnsTheQueueWithItsDerivedBracket) {
    v1::EnqueueResponse response;
    const grpc::Status status = enqueue("alice", 1300, &response);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.queue().region(), "us-west");
    EXPECT_EQ(response.queue().game_mode(), "ranked");
    EXPECT_EQ(response.queue().skill_bracket(), 2);
    EXPECT_GT(response.enqueued_at().seconds(), 0);  // a real wall-clock time
}

TEST_F(MatchmakingServiceTest, FourPlayersGetTheSameLobby) {
    v1::EnqueueResponse response;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(enqueue("p" + std::to_string(i), 1200 + i, &response).ok());
    }
    const v1::QueueKey queue = response.queue();

    // The workers match in the background, so poll like a real client would.
    v1::GetLobbyResponse lobby;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        lobby = get_lobby("p0", queue);
        if (lobby.status() == v1::PLAYER_STATUS_IN_LOBBY) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);

    ASSERT_EQ(lobby.status(), v1::PLAYER_STATUS_IN_LOBBY);
    EXPECT_EQ(lobby.lobby().lobby_id(), "node-t-1");
    EXPECT_EQ(lobby.lobby().players_size(), 4);
    EXPECT_EQ(get_lobby("p3", queue).lobby().lobby_id(), "node-t-1");
}

TEST_F(MatchmakingServiceTest, WaitingPlayerIsQueued) {
    v1::EnqueueResponse response;
    ASSERT_TRUE(enqueue("alice", 1300, &response).ok());

    EXPECT_EQ(get_lobby("alice", response.queue()).status(), v1::PLAYER_STATUS_QUEUED);
}

TEST_F(MatchmakingServiceTest, DuplicateEnqueueIsAlreadyExists) {
    v1::EnqueueResponse response;
    ASSERT_TRUE(enqueue("alice", 1300, &response).ok());

    EXPECT_EQ(enqueue("alice", 1300, &response).error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

TEST_F(MatchmakingServiceTest, MissingFieldsAreInvalidArgument) {
    v1::EnqueueResponse response;
    EXPECT_EQ(enqueue("", 1300, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(enqueue("alice", 1300, &response, "", "ranked").error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(enqueue("alice", 1300, &response, "us-west", "").error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);

    v1::GetLobbyRequest request;  // no player, no queue
    v1::GetLobbyResponse lobby;
    grpc::ClientContext context;
    EXPECT_EQ(stub_->GetLobby(&context, request, &lobby).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(MatchmakingServiceTest, CancelRemovesAWaitingPlayer) {
    v1::EnqueueResponse response;
    ASSERT_TRUE(enqueue("alice", 1300, &response).ok());

    v1::CancelRequest request;
    request.set_player_id("alice");
    *request.mutable_queue() = response.queue();

    v1::CancelResponse first;
    grpc::ClientContext first_context;
    ASSERT_TRUE(stub_->Cancel(&first_context, request, &first).ok());
    EXPECT_TRUE(first.cancelled());
    EXPECT_EQ(get_lobby("alice", response.queue()).status(), v1::PLAYER_STATUS_NOT_FOUND);

    v1::CancelResponse second;  // nothing left to cancel
    grpc::ClientContext second_context;
    ASSERT_TRUE(stub_->Cancel(&second_context, request, &second).ok());
    EXPECT_FALSE(second.cancelled());
}

TEST_F(MatchmakingServiceTest, UnknownPlayerIsNotFound) {
    v1::QueueKey queue;
    queue.set_region("us-west");
    queue.set_game_mode("ranked");
    queue.set_skill_bracket(2);

    EXPECT_EQ(get_lobby("nobody", queue).status(), v1::PLAYER_STATUS_NOT_FOUND);
}

}  // namespace
}  // namespace matchmesh::node
