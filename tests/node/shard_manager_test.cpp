// Unit tests for ShardManager on its own: when a new owner may start serving,
// and which Replicate messages it accepts. No peers are running; the cluster
// tests cover the same rules end to end.

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/node/convert.hpp"
#include "matchmesh/node/replication_client.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/node/shard_manager.hpp"

namespace matchmesh::node {
namespace {

using namespace std::chrono_literals;

template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return condition();
}

constexpr std::uint64_t kMyIncarnation = 7;

const core::MembershipView kEpoch1{1,
                                   {"node-a", "node-b", "node-c"},
                                   {{"node-a", kMyIncarnation}, {"node-b", 8}, {"node-c", 9}}};

class ShardManagerTest : public ::testing::Test {
protected:
    ShardManagerTest()
        : router_("node-a", {{"node-b", "unix:/nonexistent/b"}, {"node-c", "unix:/nonexistent/c"}}),
          replication_(router_, 50ms),
          engine_("node-a", core::EngineConfig{.worker_count = 0}, core::Clock::now,
                  &replication_),
          shards_(engine_, router_, replication_, kMyIncarnation,
                  {.retry_interval = 5ms, .log_changes = false}) {
        shards_.start();
        adopt(kEpoch1);
    }

    void adopt(const core::MembershipView& view) {
        ASSERT_TRUE(router_.apply_view(view));
        shards_.on_view_changed();
        ASSERT_TRUE(eventually([&] { return shards_.synced_epoch() == view.epoch; }));
    }

    // A queue whose primary and backup are the given nodes.
    core::QueueKey queue_placed(const std::string& primary, const std::string& backup) const {
        for (int i = 0;; ++i) {
            const core::QueueKey queue = core::make_queue_key("r" + std::to_string(i), "ranked", 1300);
            const Placement place = router_.placement(queue);
            if (place.primary == primary && place.backup == backup) {
                return queue;
            }
        }
    }

    v1::ReplicateRequest join(const core::QueueKey& queue, const std::string& sender,
                              std::uint64_t epoch, std::uint64_t sequence,
                              const std::string& player) const {
        v1::ReplicateRequest request;
        request.set_sender_node_id(sender);
        request.set_epoch(epoch);
        to_proto(core::ShardChange{queue, sequence,
                                   core::PlayerJoined{{player, 1300}, core::Clock::now()}},
                 &request);
        return request;
    }

    v1::ReplicateResponse handle(const v1::ReplicateRequest& request) {
        v1::ReplicateResponse response;
        shards_.handle(request, &response);
        return response;
    }

    Router router_;
    ReplicationClient replication_;
    core::MatchmakingEngine engine_;
    ShardManager shards_;
};

// ---- When a new owner may serve ----

TEST_F(ShardManagerTest, SettledOnlyOnceEveryMemberHasFinishedHandingOver) {
    EXPECT_FALSE(shards_.settled(kEpoch1));  // we're done; the others haven't said so

    shards_.record_progress("node-b", 1);
    EXPECT_FALSE(shards_.settled(kEpoch1));
    shards_.record_progress("node-c", 0);  // still on the previous epoch
    EXPECT_FALSE(shards_.settled(kEpoch1));
    shards_.record_progress("node-c", 1);
    EXPECT_TRUE(shards_.settled(kEpoch1));

    shards_.record_progress("node-c", std::nullopt);  // node-c restarted
    EXPECT_FALSE(shards_.settled(kEpoch1));
}

TEST_F(ShardManagerTest, ARestartedProcessIsNotSettledOnAViewThatListsItsPredecessor) {
    // A view published before this process existed: it lists node-a with
    // the incarnation of the process that crashed.
    const core::MembershipView stale{2,
                                     {"node-a", "node-b", "node-c"},
                                     {{"node-a", 6}, {"node-b", 8}, {"node-c", 9}}};
    adopt(stale);
    shards_.record_progress("node-b", 2);
    shards_.record_progress("node-c", 2);
    EXPECT_FALSE(shards_.settled(stale));
}

TEST_F(ShardManagerTest, AQueueMovingHereIsRefusedUntilTheHandOverFinishes) {
    const core::QueueKey queue = queue_placed("node-a", "node-b");
    // A copy handed over by the queue's previous holder.
    engine_.install(core::ShardState{queue, 4, {{{"alice", 1300}, core::Clock::now()}}, {}, {}},
                    core::ShardRole::kReplica);

    const auto refused = shards_.admit(queue);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(engine_.shard_info(queue)->role, core::ShardRole::kReplica);

    shards_.record_progress("node-b", 1);
    shards_.record_progress("node-c", 1);
    EXPECT_FALSE(shards_.admit(queue).has_value());
    EXPECT_EQ(engine_.shard_info(queue)->role, core::ShardRole::kPrimary);
    EXPECT_EQ(engine_.lookup(queue, "alice").status, core::PlayerStatus::kQueued);
}

// ---- Which Replicate messages are accepted ----

TEST_F(ShardManagerTest, ABackupAppliesItsPrimarysChangesInOrder) {
    const core::QueueKey queue = queue_placed("node-b", "node-a");
    EXPECT_TRUE(handle(join(queue, "node-b", 1, 1, "alice")).accepted());
    const v1::ReplicateResponse gap = handle(join(queue, "node-b", 1, 3, "carol"));
    EXPECT_FALSE(gap.accepted());
    EXPECT_NE(gap.reason().find("snapshot"), std::string::npos) << gap.reason();
    EXPECT_TRUE(handle(join(queue, "node-b", 1, 2, "bob")).accepted());
    EXPECT_EQ(engine_.shard_info(queue)->queued, 2u);
}

TEST_F(ShardManagerTest, ChangesFromAnyOtherEpochAreRejected) {
    const core::QueueKey queue = queue_placed("node-b", "node-a");

    const v1::ReplicateResponse older = handle(join(queue, "node-b", 0, 1, "alice"));
    EXPECT_FALSE(older.accepted());
    EXPECT_EQ(older.epoch(), 1u);
    EXPECT_NE(older.reason().find("older"), std::string::npos) << older.reason();

    const v1::ReplicateResponse newer = handle(join(queue, "node-b", 2, 1, "alice"));
    EXPECT_FALSE(newer.accepted());

    EXPECT_FALSE(engine_.shard_info(queue).has_value()) << "nothing was applied";
}

TEST_F(ShardManagerTest, OnlyTheQueuesPrimaryMayChangeItsBackup) {
    const core::QueueKey queue = queue_placed("node-b", "node-a");
    EXPECT_FALSE(handle(join(queue, "node-c", 1, 1, "alice")).accepted());

    // And a node that isn't the queue's backup refuses its primary's changes.
    const core::QueueKey elsewhere = queue_placed("node-b", "node-c");
    EXPECT_FALSE(handle(join(elsewhere, "node-b", 1, 1, "alice")).accepted());
}

TEST_F(ShardManagerTest, AHandOverKeepsTheNewestCopyAndNeverOverwritesALiveQueue) {
    const core::QueueKey queue = queue_placed("node-a", "node-b");
    const auto hand_over = [&](const std::string& sender, std::uint64_t sequence,
                               const std::string& player) {
        v1::ReplicateRequest request;
        request.set_sender_node_id(sender);
        request.set_epoch(1);
        to_proto(core::ShardState{queue, sequence, {{{player, 1300}, core::Clock::now()}}, {}, {}},
                 &request);
        return handle(request).accepted();
    };

    EXPECT_TRUE(hand_over("node-b", 5, "from-b"));
    EXPECT_TRUE(hand_over("node-c", 4, "from-c"));  // older: accepted but not kept
    EXPECT_EQ(engine_.lookup(queue, "from-b").status, core::PlayerStatus::kQueued);
    EXPECT_EQ(engine_.lookup(queue, "from-c").status, core::PlayerStatus::kNotFound);

    engine_.set_role(queue, core::ShardRole::kPrimary);  // now live here
    EXPECT_TRUE(hand_over("node-c", 9, "late"));
    EXPECT_EQ(engine_.lookup(queue, "late").status, core::PlayerStatus::kNotFound);
    EXPECT_EQ(engine_.lookup(queue, "from-b").status, core::PlayerStatus::kQueued);
}

}  // namespace
}  // namespace matchmesh::node
