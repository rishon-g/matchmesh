// Tests for SkillQueue: ordering, duplicates, bracket checks and cancellation.

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/skill_queue.hpp"

namespace matchmesh::core {
namespace {

using namespace std::chrono_literals;

// Every test uses the us-west/ranked/2 queue (ratings 1000-1499) and a fixed
// start time, so results never depend on the real clock.
class SkillQueueTest : public ::testing::Test {
protected:
    // The queue's players' IDs, in the order the queue iterates them.
    std::vector<std::string> ids_in_order() const {
        std::vector<std::string> ids;
        for (const QueuedPlayer& entry : queue_) {
            ids.push_back(entry.player.id);
        }
        return ids;
    }

    const TimePoint t0_{};
    SkillQueue queue_{make_queue_key("us-west", "ranked", 1300)};
};

TEST_F(SkillQueueTest, StartsEmpty) {
    EXPECT_TRUE(queue_.empty());
    EXPECT_EQ(queue_.size(), 0u);
    EXPECT_EQ(queue_.begin(), queue_.end());
    EXPECT_FALSE(queue_.contains("alice"));
}

TEST_F(SkillQueueTest, ExposesItsKey) {
    EXPECT_EQ(queue_.key(), make_queue_key("us-west", "ranked", 1300));
}

TEST_F(SkillQueueTest, EnqueueAddsThePlayer) {
    EXPECT_EQ(queue_.enqueue({"alice", 1300}, t0_), EnqueueResult::kAdded);

    EXPECT_FALSE(queue_.empty());
    EXPECT_EQ(queue_.size(), 1u);
    EXPECT_TRUE(queue_.contains("alice"));
}

TEST_F(SkillQueueTest, RecordsWhenEachPlayerJoined) {
    ASSERT_EQ(queue_.enqueue({"alice", 1300}, t0_ + 7s), EnqueueResult::kAdded);
    EXPECT_EQ(queue_.begin()->enqueued_at, t0_ + 7s);
}

TEST_F(SkillQueueTest, KeepsPlayersSortedByRating) {
    ASSERT_EQ(queue_.enqueue({"cara", 1300}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"alice", 1100}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"dev", 1450}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"bob", 1200}, t0_), EnqueueResult::kAdded);

    EXPECT_EQ(ids_in_order(), (std::vector<std::string>{"alice", "bob", "cara", "dev"}));
}

TEST_F(SkillQueueTest, EqualRatingsKeepArrivalOrder) {
    ASSERT_EQ(queue_.enqueue({"first", 1300}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"second", 1300}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"third", 1300}, t0_), EnqueueResult::kAdded);

    EXPECT_EQ(ids_in_order(), (std::vector<std::string>{"first", "second", "third"}));
}

TEST_F(SkillQueueTest, RejectsDuplicatePlayers) {
    ASSERT_EQ(queue_.enqueue({"alice", 1300}, t0_), EnqueueResult::kAdded);

    // Same ID is a duplicate even if the rating differs.
    EXPECT_EQ(queue_.enqueue({"alice", 1300}, t0_), EnqueueResult::kDuplicate);
    EXPECT_EQ(queue_.enqueue({"alice", 1250}, t0_ + 1s), EnqueueResult::kDuplicate);

    // The original entry is untouched.
    EXPECT_EQ(queue_.size(), 1u);
    EXPECT_EQ(queue_.begin()->player.rating, 1300);
    EXPECT_EQ(queue_.begin()->enqueued_at, t0_);
}

TEST_F(SkillQueueTest, RejectsRatingsFromOtherBrackets) {
    EXPECT_EQ(queue_.enqueue({"too-low", 999}, t0_), EnqueueResult::kWrongBracket);
    EXPECT_EQ(queue_.enqueue({"too-high", 1500}, t0_), EnqueueResult::kWrongBracket);
    EXPECT_TRUE(queue_.empty());

    // The bracket's own edges are accepted.
    EXPECT_EQ(queue_.enqueue({"bottom", 1000}, t0_), EnqueueResult::kAdded);
    EXPECT_EQ(queue_.enqueue({"top", 1499}, t0_), EnqueueResult::kAdded);
}

TEST_F(SkillQueueTest, RejectsEmptyPlayerId) {
    EXPECT_EQ(queue_.enqueue({"", 1300}, t0_), EnqueueResult::kInvalidPlayer);
    EXPECT_TRUE(queue_.empty());
}

TEST_F(SkillQueueTest, CancelRemovesThePlayerAndReturnsThem) {
    ASSERT_EQ(queue_.enqueue({"alice", 1300}, t0_ + 2s), EnqueueResult::kAdded);

    const auto removed = queue_.remove("alice");

    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->player, (Player{"alice", 1300}));
    EXPECT_EQ(removed->enqueued_at, t0_ + 2s);
    EXPECT_TRUE(queue_.empty());
    EXPECT_FALSE(queue_.contains("alice"));
}

TEST_F(SkillQueueTest, CancellingAnUnknownPlayerDoesNothing) {
    ASSERT_EQ(queue_.enqueue({"alice", 1300}, t0_), EnqueueResult::kAdded);

    EXPECT_FALSE(queue_.remove("nobody").has_value());
    EXPECT_EQ(queue_.size(), 1u);
}

TEST_F(SkillQueueTest, CancellingOnAnEmptyQueueDoesNothing) {
    EXPECT_FALSE(queue_.remove("alice").has_value());
    EXPECT_TRUE(queue_.empty());
}

TEST_F(SkillQueueTest, CancellingTwiceOnlyRemovesOnce) {
    ASSERT_EQ(queue_.enqueue({"alice", 1300}, t0_), EnqueueResult::kAdded);

    EXPECT_TRUE(queue_.remove("alice").has_value());
    EXPECT_FALSE(queue_.remove("alice").has_value());
}

TEST_F(SkillQueueTest, CancelLeavesTheRestSorted) {
    ASSERT_EQ(queue_.enqueue({"alice", 1100}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"bob", 1200}, t0_), EnqueueResult::kAdded);
    ASSERT_EQ(queue_.enqueue({"cara", 1300}, t0_), EnqueueResult::kAdded);

    ASSERT_TRUE(queue_.remove("bob").has_value());

    EXPECT_EQ(ids_in_order(), (std::vector<std::string>{"alice", "cara"}));
}

TEST_F(SkillQueueTest, PlayerCanRejoinAfterCancelling) {
    ASSERT_EQ(queue_.enqueue({"alice", 1300}, t0_), EnqueueResult::kAdded);
    ASSERT_TRUE(queue_.remove("alice").has_value());

    EXPECT_EQ(queue_.enqueue({"alice", 1350}, t0_ + 5s), EnqueueResult::kAdded);
    EXPECT_EQ(queue_.begin()->player.rating, 1350);
    EXPECT_EQ(queue_.begin()->enqueued_at, t0_ + 5s);
}

}  // namespace
}  // namespace matchmesh::core
