// Tests for the engine's side of primary-backup replication: every change is
// copied to the backup before it is applied, replicas apply changes strictly
// in order, and a full copy can move a shard to another engine intact.

#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/replication.hpp"

namespace matchmesh::core {
namespace {

using namespace std::chrono_literals;

// Stands in for the network. Records every call and, if `backup` is set,
// applies each change to it exactly as a real backup node would (falling
// back to a full copy when the backup can't apply the change in order).
class FakeReplicator final : public Replicator {
public:
    bool replicate(const ShardChange& change,
                   const std::function<ShardState()>& current) override {
        changes.push_back(change);
        if (before_accepting) {
            before_accepting(change);
        }
        if (!accept) {
            return false;
        }
        if (backup != nullptr && !backup->apply_replicated(change)) {
            backup->install(current(), ShardRole::kReplica);
            if (!backup->apply_replicated(change)) {
                return false;
            }
        }
        return true;
    }

    bool sync(const QueueKey& queue, const std::function<ShardState()>& current) override {
        synced.push_back(queue);
        if (accept && backup != nullptr) {
            backup->install(current(), ShardRole::kReplica);
        }
        return accept;
    }

    bool accept = true;
    MatchmakingEngine* backup = nullptr;
    std::function<void(const ShardChange&)> before_accepting;
    std::vector<ShardChange> changes;
    std::vector<QueueKey> synced;
};

std::set<std::string> ids(const std::vector<Player>& players) {
    std::set<std::string> out;
    for (const Player& player : players) {
        out.insert(player.id);
    }
    return out;
}

// Two shard copies hold the same thing: same sequence, the same players
// waiting in the same order since the same moment, and the same lobbies.
void expect_same(const ShardState& a, const ShardState& b) {
    EXPECT_EQ(a.queue, b.queue);
    EXPECT_EQ(a.sequence, b.sequence);
    ASSERT_EQ(a.waiting.size(), b.waiting.size());
    for (std::size_t i = 0; i < a.waiting.size(); ++i) {
        EXPECT_EQ(a.waiting[i].player, b.waiting[i].player);
        EXPECT_EQ(a.waiting[i].enqueued_at, b.waiting[i].enqueued_at);
    }
    ASSERT_EQ(a.lobbies.size(), b.lobbies.size());
    for (std::size_t i = 0; i < a.lobbies.size(); ++i) {
        EXPECT_EQ(a.lobbies[i].lobby_id, b.lobbies[i].lobby_id);
        EXPECT_EQ(a.lobbies[i].players, b.lobbies[i].players);
    }
    EXPECT_EQ(a.lobby_of, b.lobby_of);
}

class ReplicationTest : public ::testing::Test {
protected:
    ReplicationTest()
        : primary_("node-a", EngineConfig{.worker_count = 0}, [this] { return now_; },
                   &replicator_),
          backup_("node-b", EngineConfig{.worker_count = 0}, [this] { return now_; }) {}

    void enqueue_four(int base_rating) {
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(primary_.enqueue({"p" + std::to_string(base_rating + i), base_rating + i},
                                       "us-west", "ranked")
                          .result,
                      EnqueueResult::kAdded);
        }
    }

    TimePoint now_{};
    FakeReplicator replicator_;
    MatchmakingEngine primary_;
    MatchmakingEngine backup_;
    const QueueKey ranked2_ = make_queue_key("us-west", "ranked", 1200);
};

// ---- The primary: propose, copy, then apply ----

TEST_F(ReplicationTest, AChangeIsCopiedBeforeItIsApplied) {
    PlayerStatus status_while_copying = PlayerStatus::kQueued;
    replicator_.before_accepting = [&](const ShardChange&) {
        status_while_copying = primary_.lookup(ranked2_, "alice").status;
    };

    ASSERT_EQ(primary_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);

    EXPECT_EQ(status_while_copying, PlayerStatus::kNotFound);  // not visible until copied
    EXPECT_EQ(primary_.lookup(ranked2_, "alice").status, PlayerStatus::kQueued);
    ASSERT_EQ(replicator_.changes.size(), 1u);
    const auto& joined = std::get<PlayerJoined>(replicator_.changes[0].op);
    EXPECT_EQ(joined.player, (Player{"alice", 1300}));
    EXPECT_EQ(replicator_.changes[0].sequence, 1u);
}

TEST_F(ReplicationTest, AFailedCopyChangesNothing) {
    replicator_.accept = false;
    EXPECT_EQ(primary_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kNotReplicated);
    EXPECT_EQ(primary_.lookup(ranked2_, "alice").status, PlayerStatus::kNotFound);
    EXPECT_EQ(primary_.shard_info(ranked2_)->sequence, 0u);

    // Retrying once the backup is back works, and reuses the sequence number
    // because the failed change never happened.
    replicator_.accept = true;
    EXPECT_EQ(primary_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    EXPECT_EQ(replicator_.changes.back().sequence, 1u);
}

TEST_F(ReplicationTest, CancelsAreCopiedButNoOpsAreNot) {
    ASSERT_EQ(primary_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    EXPECT_EQ(primary_.cancel(ranked2_, "bob"), CancelResult::kNotWaiting);
    EXPECT_EQ(replicator_.changes.size(), 1u);  // nothing to copy

    replicator_.accept = false;
    EXPECT_EQ(primary_.cancel(ranked2_, "alice"), CancelResult::kNotReplicated);
    EXPECT_EQ(primary_.lookup(ranked2_, "alice").status, PlayerStatus::kQueued);

    replicator_.accept = true;
    EXPECT_EQ(primary_.cancel(ranked2_, "alice"), CancelResult::kCancelled);
    EXPECT_EQ(std::get<PlayerLeft>(replicator_.changes.back().op).player_id, "alice");
}

TEST_F(ReplicationTest, ALobbyIsCopiedAsOneChange) {
    enqueue_four(1200);
    ASSERT_EQ(primary_.run_match_pass(now_), 1u);

    ASSERT_EQ(replicator_.changes.size(), 5u);  // four joins, then the lobby
    const ShardChange& last = replicator_.changes.back();
    EXPECT_EQ(last.sequence, 5u);
    const Lobby& lobby = std::get<LobbyFormed>(last.op).lobby;
    EXPECT_EQ(ids(lobby.players), (std::set<std::string>{"p1200", "p1201", "p1202", "p1203"}));
    EXPECT_FALSE(lobby.lobby_id.empty());
    EXPECT_EQ(primary_.lookup(ranked2_, "p1200").lobby->lobby_id, lobby.lobby_id);
}

TEST_F(ReplicationTest, ALobbyThatCannotBeCopiedIsNotFormed) {
    enqueue_four(1200);
    replicator_.accept = false;

    EXPECT_EQ(primary_.run_match_pass(now_), 0u);
    for (const char* id : {"p1200", "p1201", "p1202", "p1203"}) {
        EXPECT_EQ(primary_.lookup(ranked2_, id).status, PlayerStatus::kQueued) << id;
    }
    EXPECT_EQ(primary_.lobbies_formed(), 0u);

    replicator_.accept = true;  // the next pass simply tries again
    EXPECT_EQ(primary_.run_match_pass(now_), 1u);
}

// ---- The replica: strictly in order ----

TEST_F(ReplicationTest, AReplicaAppliesChangesOnlyInOrder) {
    const auto join = [&](std::uint64_t sequence, const std::string& id) {
        return ShardChange{ranked2_, sequence, PlayerJoined{{id, 1300}, now_}};
    };
    EXPECT_TRUE(backup_.apply_replicated(join(1, "alice")));
    EXPECT_FALSE(backup_.apply_replicated(join(3, "carol")));  // a gap: 2 is missing
    EXPECT_TRUE(backup_.apply_replicated(join(2, "bob")));
    EXPECT_FALSE(backup_.apply_replicated(join(2, "bob")));    // a repeat
    EXPECT_TRUE(backup_.apply_replicated(join(3, "carol")));

    EXPECT_EQ(backup_.shard_info(ranked2_)->sequence, 3u);
    EXPECT_EQ(backup_.shard_info(ranked2_)->role, ShardRole::kReplica);
    EXPECT_EQ(backup_.lookup(ranked2_, "bob").status, PlayerStatus::kQueued);
}

TEST_F(ReplicationTest, ALobbyThatDoesNotFitTheReplicaIsRejectedWhole) {
    for (std::uint64_t i = 1; i <= 3; ++i) {
        ASSERT_TRUE(backup_.apply_replicated(
            ShardChange{ranked2_, i, PlayerJoined{{"p" + std::to_string(i), 1300}, now_}}));
    }
    Lobby lobby{"node-a-1", ranked2_, {{"p1", 1300}, {"p2", 1300}, {"p3", 1300}, {"p4", 1300}},
                now_};
    EXPECT_FALSE(backup_.apply_replicated(ShardChange{ranked2_, 4, LobbyFormed{lobby}}));

    for (const char* id : {"p1", "p2", "p3"}) {  // nobody was half-moved into it
        EXPECT_EQ(backup_.lookup(ranked2_, id).status, PlayerStatus::kQueued) << id;
    }
}

TEST_F(ReplicationTest, ReplicasAreNotMatchedAndRefuseClientChanges) {
    replicator_.backup = &backup_;
    enqueue_four(1200);
    ASSERT_EQ(backup_.shard_info(ranked2_)->queued, 4u);

    EXPECT_EQ(backup_.run_match_pass(now_), 0u);  // only the primary matches
    EXPECT_EQ(backup_.enqueue({"zoe", 1300}, "us-west", "ranked").result,
              EnqueueResult::kNotPrimary);
    EXPECT_EQ(backup_.cancel(ranked2_, "p1200"), CancelResult::kNotPrimary);

    backup_.set_role(ranked2_, ShardRole::kPrimary);  // failover
    EXPECT_EQ(backup_.run_match_pass(now_), 1u);
}

// ---- Whole copies ----

TEST_F(ReplicationTest, AMirroredBackupEndsUpIdentical) {
    replicator_.backup = &backup_;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> rating(1000, 1499);  // all one bracket
    std::uniform_int_distribution<int> action(0, 9);

    std::vector<std::string> joined;
    for (int step = 0; step < 400; ++step) {
        now_ += 100ms;
        const int roll = action(rng);
        if (roll < 6) {
            const std::string id = "p" + std::to_string(step);
            ASSERT_EQ(primary_.enqueue({id, rating(rng)}, "us-west", "ranked").result,
                      EnqueueResult::kAdded);
            joined.push_back(id);
        } else if (roll < 8 && !joined.empty()) {
            (void)primary_.cancel(make_queue_key("us-west", "ranked", 1000),
                                  joined[static_cast<std::size_t>(step) % joined.size()]);
        } else {
            (void)primary_.run_match_pass(now_);
        }
    }

    const QueueKey queue = make_queue_key("us-west", "ranked", 1000);
    ASSERT_GT(primary_.lobbies_formed(), 10u);
    expect_same(*primary_.snapshot(queue), *backup_.snapshot(queue));
}

TEST_F(ReplicationTest, APromotedCopyCarriesOnExactlyWhereThePrimaryStopped) {
    replicator_.backup = &backup_;
    // Ratings too far apart to match at first; after 20 s of widening they fit.
    for (const int r : {1000, 1060, 1120, 1180}) {
        ASSERT_EQ(primary_.enqueue({"p" + std::to_string(r), r}, "us-west", "ranked").result,
                  EnqueueResult::kAdded);
    }
    ASSERT_EQ(primary_.run_match_pass(now_), 0u);

    // The primary dies; its backup takes over and inherits the wait times.
    backup_.set_role(make_queue_key("us-west", "ranked", 1000), ShardRole::kPrimary);
    EXPECT_EQ(backup_.run_match_pass(now_ + 5s), 0u);
    EXPECT_EQ(backup_.run_match_pass(now_ + 20s), 1u);
}

TEST_F(ReplicationTest, InstallKeepsWaitTimesArrivalOrderAndLobbies) {
    enqueue_four(1200);
    ASSERT_EQ(primary_.run_match_pass(now_), 1u);
    now_ += 10s;
    // Two equal ratings: whoever arrived first is ahead in the tie-break.
    ASSERT_EQ(primary_.enqueue({"first", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    ASSERT_EQ(primary_.enqueue({"second", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);

    const ShardState state = *primary_.snapshot(ranked2_);
    ASSERT_EQ(state.waiting.size(), 2u);
    EXPECT_EQ(state.waiting[0].player.id, "first");
    EXPECT_EQ(state.sequence, 7u);

    backup_.install(state, ShardRole::kReplica);
    expect_same(state, *backup_.snapshot(ranked2_));
    EXPECT_EQ(backup_.lookup(ranked2_, "p1201").lobby->lobby_id,
              primary_.lookup(ranked2_, "p1201").lobby->lobby_id);
}

TEST_F(ReplicationTest, APlayerWhoQueuedAgainIsNotListedInTheirOldLobby) {
    enqueue_four(1200);
    ASSERT_EQ(primary_.run_match_pass(now_), 1u);
    ASSERT_EQ(primary_.enqueue({"p1200", 1200}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    ASSERT_EQ(primary_.cancel(ranked2_, "p1200"), CancelResult::kCancelled);

    backup_.install(*primary_.snapshot(ranked2_), ShardRole::kReplica);
    EXPECT_EQ(backup_.lookup(ranked2_, "p1200").status, PlayerStatus::kNotFound);
    EXPECT_EQ(backup_.lookup(ranked2_, "p1201").status, PlayerStatus::kInLobby);
}

TEST_F(ReplicationTest, DroppedQueuesAreForgotten) {
    ASSERT_EQ(primary_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    ASSERT_EQ(primary_.shards().size(), 1u);

    primary_.drop(ranked2_);
    EXPECT_TRUE(primary_.shards().empty());
    EXPECT_FALSE(primary_.shard_info(ranked2_).has_value());
    EXPECT_FALSE(primary_.snapshot(ranked2_).has_value());
    EXPECT_EQ(primary_.lookup(ranked2_, "alice").status, PlayerStatus::kNotFound);

    // A later enqueue starts the queue afresh, from change 1.
    ASSERT_EQ(primary_.enqueue({"bob", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    EXPECT_EQ(replicator_.changes.back().sequence, 1u);
}

TEST_F(ReplicationTest, OnlyPrimariesSyncTheirBackup) {
    replicator_.backup = &backup_;
    ASSERT_EQ(primary_.enqueue({"alice", 1300}, "us-west", "ranked").result,
              EnqueueResult::kAdded);
    EXPECT_TRUE(primary_.sync_backup(ranked2_));
    EXPECT_EQ(replicator_.synced.size(), 1u);

    EXPECT_FALSE(backup_.sync_backup(ranked2_));  // a replica has no backup of its own
    EXPECT_FALSE(primary_.sync_backup(make_queue_key("eu", "casual", 0)));  // not held
}

}  // namespace
}  // namespace matchmesh::core
