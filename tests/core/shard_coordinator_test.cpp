// Tests for core::ShardCoordinator and core::BackupReplicator with the
// network replaced by direct calls between engines in one process. The
// node-level tests (tests/node/shard_manager_test.cpp, cluster_test.cpp) run
// the same logic over real gRPC.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/placement.hpp"
#include "matchmesh/core/shard_coordinator.hpp"

namespace matchmesh::core {
namespace {

class TestNode;

// Every node, by ID. A node missing from it (or marked down) is unreachable.
struct TestNetwork {
    std::map<std::string, TestNode*> nodes;
    std::map<std::string, bool> down;
};

// One node: its view of the cluster, an engine, and a coordinator, with
// messages delivered straight to the receiver's coordinator.
class TestNode final : public PlacementSource, public ReplicaTransport {
public:
    TestNode(std::string id, TestNetwork& network)
        : id_(std::move(id)),
          network_(network),
          backups_(*this, *this),
          engine_(id_ + "-1", EngineConfig{.worker_count = 0}, Clock::now, &backups_),
          coordinator_(engine_, *this, *this, 1) {
        network_.nodes[id_] = this;
    }

    void adopt(MembershipView view) {
        HashRing ring;
        for (const auto& member : view.members) {
            ring.add_node(member);
        }
        view_ = std::move(view);
        ring_ = std::move(ring);
    }

    const std::string& self_id() const override { return id_; }
    Placement placement(const QueueKey& queue) const override {
        return place(ring_, view_.epoch, queue);
    }
    MembershipView view() const override { return view_; }

    bool send_change(const std::string& target, std::uint64_t epoch,
                     const ShardChange& change) override {
        TestNode* receiver = reachable(target);
        return receiver != nullptr && receiver->coordinator_.receive(id_, epoch, change).accepted;
    }
    bool send_snapshot(const std::string& target, std::uint64_t epoch,
                       const ShardState& state) override {
        TestNode* receiver = reachable(target);
        return receiver != nullptr && receiver->coordinator_.receive(id_, epoch, state).accepted;
    }

    MatchmakingEngine& engine() { return engine_; }
    ShardCoordinator& coordinator() { return coordinator_; }

private:
    TestNode* reachable(const std::string& target) const {
        const auto it = network_.nodes.find(target);
        return it == network_.nodes.end() || network_.down[target] ? nullptr : it->second;
    }

    std::string id_;
    TestNetwork& network_;
    MembershipView view_;
    HashRing ring_;
    BackupReplicator backups_;
    MatchmakingEngine engine_;
    ShardCoordinator coordinator_;
};

MembershipView view_of(std::uint64_t epoch, std::vector<std::string> members) {
    MembershipView view{epoch, std::move(members)};
    for (const auto& member : view.members) {
        view.incarnations[member] = 1;
    }
    return view;
}

class ShardCoordinatorTest : public ::testing::Test {
protected:
    ShardCoordinatorTest() {
        for (const char* id : {"node-a", "node-b", "node-c"}) {
            nodes_.push_back(std::make_unique<TestNode>(id, network_));
        }
        everyone_adopts(view_of(1, {"node-a", "node-b", "node-c"}));
    }

    void everyone_adopts(const MembershipView& view) {
        for (auto& node : nodes_) {
            if (!network_.down[node->self_id()]) {
                node->adopt(view);
            }
        }
    }

    // Runs coordinator passes and passes progress around, like heartbeats.
    void settle(int rounds = 4) {
        for (int i = 0; i < rounds; ++i) {
            for (auto& node : nodes_) {
                if (!network_.down[node->self_id()]) {
                    (void)node->coordinator().run_pass();
                }
            }
            for (auto& from : nodes_) {
                for (auto& to : nodes_) {
                    if (from != to && !network_.down[from->self_id()]) {
                        to->coordinator().record_progress(from->self_id(),
                                                          from->coordinator().synced_epoch());
                    }
                }
            }
        }
    }

    TestNode& node(const std::string& id) { return *network_.nodes.at(id); }

    TestNetwork network_;
    std::vector<std::unique_ptr<TestNode>> nodes_;
    const QueueKey queue_{"us-west", "ranked", 1};
};

TEST_F(ShardCoordinatorTest, ChangesReachTheBackupBeforeThePlayerIsTold) {
    settle();
    const Placement place = node("node-a").placement(queue_);
    TestNode& primary = node(place.primary);
    ASSERT_FALSE(primary.coordinator().admit(queue_).has_value());
    EXPECT_EQ(primary.engine().enqueue({"p1", 700}, "us-west", "ranked").result,
              EnqueueResult::kAdded);

    const auto copy = node(*place.backup).engine().shard_info(queue_);
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(copy->role, ShardRole::kReplica);
    EXPECT_EQ(copy->queued, 1U);
}

TEST_F(ShardCoordinatorTest, AnUnreachableBackupMeansNothingChanges) {
    settle();
    const Placement place = node("node-a").placement(queue_);
    network_.down[*place.backup] = true;
    EXPECT_EQ(node(place.primary).engine().enqueue({"p1", 700}, "us-west", "ranked").result,
              EnqueueResult::kNotReplicated);
}

TEST_F(ShardCoordinatorTest, RefusesAnythingFromTheWrongEpochOrSender) {
    settle();
    const Placement place = node("node-a").placement(queue_);
    TestNode& backup = node(*place.backup);
    ShardState empty;
    empty.queue = queue_;

    EXPECT_FALSE(backup.coordinator().receive(place.primary, 0, empty).accepted);  // older
    EXPECT_FALSE(backup.coordinator().receive(place.primary, 2, empty).accepted);  // newer
    std::string stranger = "node-a";
    for (const auto& id : {"node-a", "node-b", "node-c"}) {
        if (id != place.primary && id != *place.backup) {
            stranger = id;
        }
    }
    const ReplicaVerdict verdict = backup.coordinator().receive(stranger, 1, empty);
    EXPECT_FALSE(verdict.accepted);
    EXPECT_EQ(verdict.epoch, 1U);
    EXPECT_TRUE(backup.coordinator().receive(place.primary, 1, empty).accepted);
}

TEST_F(ShardCoordinatorTest, TheBackupTakesOverWithEveryPlayerOnceSettled) {
    settle();
    const Placement before = node("node-a").placement(queue_);
    TestNode& primary = node(before.primary);
    ASSERT_FALSE(primary.coordinator().admit(queue_).has_value());
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(primary.engine()
                      .enqueue({"p" + std::to_string(i), 700}, "us-west", "ranked")
                      .result,
                  EnqueueResult::kAdded);
    }

    // The primary dies; the survivors agree on a view without it.
    network_.down[before.primary] = true;
    std::vector<std::string> survivors;
    for (const auto& id : {"node-a", "node-b", "node-c"}) {
        if (id != before.primary) {
            survivors.emplace_back(id);
        }
    }
    everyone_adopts(view_of(2, survivors));
    TestNode& heir = node(*before.backup);
    ASSERT_EQ(heir.placement(queue_).primary, *before.backup);
    EXPECT_TRUE(heir.coordinator().admit(queue_).has_value()) << "must wait for the hand-over";

    settle();
    EXPECT_FALSE(heir.coordinator().admit(queue_).has_value());
    const auto info = heir.engine().shard_info(queue_);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->role, ShardRole::kPrimary);
    EXPECT_EQ(info->queued, 3U);
    ASSERT_EQ(heir.coordinator().served_shards().size(), 1U);
}

}  // namespace
}  // namespace matchmesh::core
