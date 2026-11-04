// Tests for the gateway's poller against real gRPC servers in-process: it
// asks every node at once, merges the answers, and only reports changes.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <crow/json.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "matchmesh/gateway/poller.hpp"

namespace matchmesh::gateway {
namespace {

using namespace std::chrono_literals;

// A node that answers GetTopology with whatever the test sets.
class FakeNode final : public v1::ClusterService::Service {
public:
    grpc::Status GetTopology(grpc::ServerContext*, const v1::GetTopologyRequest*,
                             v1::GetTopologyResponse* response) override {
        const std::scoped_lock lock(mutex_);
        *response = answer_;
        return grpc::Status::OK;
    }
    void set_epoch(std::uint64_t epoch) {
        const std::scoped_lock lock(mutex_);
        answer_.set_epoch(epoch);
    }

private:
    std::mutex mutex_;
    v1::GetTopologyResponse answer_;
};

class PollerTest : public ::testing::Test {
protected:
    PollerTest()
        : dir_(std::filesystem::temp_directory_path() / ("mm-gw-" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(dir_);
        address_ = "unix:" + (dir_ / "a.sock").string();
        grpc::ServerBuilder builder;
        builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
        builder.RegisterService(&node_);
        server_ = builder.BuildAndStart();
        node_.set_epoch(7);
    }
    ~PollerTest() override {
        server_->Shutdown(std::chrono::system_clock::now());
        std::filesystem::remove_all(dir_);
    }

    std::filesystem::path dir_;
    std::string address_;
    FakeNode node_;
    std::unique_ptr<grpc::Server> server_;
};

TEST_F(PollerTest, MergesAnswersAndMarksNodesThatDontAnswer) {
    TopologyPoller poller({{"node-a", address_}, {"node-z", "unix:" + (dir_ / "none.sock").string()}},
                          nullptr, 50ms, 200ms);
    const auto doc = crow::json::load(poller.poll_once());
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc["epoch"].u(), 7u);
    EXPECT_TRUE(doc["nodes"][0]["reachable"].b());
    EXPECT_FALSE(doc["nodes"][1]["reachable"].b());
    EXPECT_EQ(poller.latest(), poller.poll_once());
}

// A node that has hung: it accepts the call but never answers in time.
class StuckNode final : public v1::ClusterService::Service {
public:
    grpc::Status GetTopology(grpc::ServerContext*, const v1::GetTopologyRequest*,
                             v1::GetTopologyResponse*) override {
        std::this_thread::sleep_for(1s);
        return grpc::Status::OK;
    }
};

TEST_F(PollerTest, StuckNodesCostOneTimeoutPerRoundNotOneEach) {
    // Four hung nodes and one healthy one. Asked one after another, a round
    // would take four 300 ms timeouts (1.2 s); asked all at once, about one.
    StuckNode stuck;
    grpc::ServerBuilder builder;
    std::map<std::string, std::string> nodes = {{"node-a", address_}};
    for (int i = 0; i < 4; ++i) {
        const std::string address = "unix:" + (dir_ / ("stuck" + std::to_string(i) + ".sock")).string();
        builder.AddListeningPort(address, grpc::InsecureServerCredentials());
        nodes["stuck-" + std::to_string(i)] = address;
    }
    builder.RegisterService(&stuck);
    const auto stuck_server = builder.BuildAndStart();
    TopologyPoller poller(nodes, nullptr, 50ms, 300ms);

    const auto started = std::chrono::steady_clock::now();
    const auto doc = crow::json::load(poller.poll_once());
    const auto took = std::chrono::steady_clock::now() - started;

    EXPECT_LT(took, 700ms) << std::chrono::duration_cast<std::chrono::milliseconds>(took).count();
    ASSERT_TRUE(doc);
    EXPECT_TRUE(doc["nodes"][0]["reachable"].b());  // the healthy node still made it in
    EXPECT_FALSE(doc["nodes"][1]["reachable"].b());
    stuck_server->Shutdown(std::chrono::system_clock::now());
}

TEST_F(PollerTest, TheListenerHearsOnlyAboutChanges) {
    std::atomic<int> heard{0};
    std::mutex last_mutex;
    std::string last;
    TopologyPoller poller({{"node-a", address_}},
                          [&](const std::string& json) {
                              const std::scoped_lock lock(last_mutex);
                              last = json;
                              ++heard;
                          },
                          20ms, 200ms);
    poller.start();
    std::this_thread::sleep_for(300ms);  // ~15 rounds, one change
    EXPECT_EQ(heard.load(), 1);

    node_.set_epoch(8);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (heard.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    poller.stop();
    EXPECT_EQ(heard.load(), 2);
    const std::scoped_lock lock(last_mutex);
    EXPECT_EQ(crow::json::load(last)["epoch"].u(), 8u);
}

}  // namespace
}  // namespace matchmesh::gateway
