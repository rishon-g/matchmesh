// Tests for the basic types: skill brackets and queue keys.

#include <unordered_map>

#include <gtest/gtest.h>

#include "matchmesh/core/types.hpp"

namespace matchmesh::core {
namespace {

TEST(SkillBracketTest, SplitsRatingsInto500PointBands) {
    EXPECT_EQ(skill_bracket_for(0), 0);
    EXPECT_EQ(skill_bracket_for(499), 0);
    EXPECT_EQ(skill_bracket_for(500), 1);
    EXPECT_EQ(skill_bracket_for(1300), 2);
    EXPECT_EQ(skill_bracket_for(1499), 2);
    EXPECT_EQ(skill_bracket_for(1500), 3);
}

TEST(SkillBracketTest, NegativeRatingsFallIntoBracketZero) {
    EXPECT_EQ(skill_bracket_for(-1), 0);
    EXPECT_EQ(skill_bracket_for(-750), 0);
}

TEST(QueueKeyTest, MakeQueueKeyDerivesBracketFromRating) {
    const QueueKey key = make_queue_key("us-west", "ranked", 1300);
    EXPECT_EQ(key.region, "us-west");
    EXPECT_EQ(key.game_mode, "ranked");
    EXPECT_EQ(key.skill_bracket, 2);
}

TEST(QueueKeyTest, ToStringJoinsFieldsWithSlashes) {
    EXPECT_EQ(to_string(make_queue_key("us-west", "ranked", 1300)), "us-west/ranked/2");
}

TEST(QueueKeyTest, PlayersInTheSameBandShareAQueue) {
    EXPECT_EQ(make_queue_key("eu", "casual", 1000), make_queue_key("eu", "casual", 1499));
    EXPECT_NE(make_queue_key("eu", "casual", 1499), make_queue_key("eu", "casual", 1500));
    EXPECT_NE(make_queue_key("eu", "casual", 1200), make_queue_key("eu", "ranked", 1200));
    EXPECT_NE(make_queue_key("eu", "casual", 1200), make_queue_key("us-west", "casual", 1200));
}

TEST(QueueKeyTest, WorksAsAnUnorderedMapKey) {
    std::unordered_map<QueueKey, int, QueueKeyHash> queues;
    queues[make_queue_key("us-west", "ranked", 1300)] = 1;
    queues[make_queue_key("us-west", "ranked", 1700)] = 2;

    EXPECT_EQ(queues.size(), 2u);
    EXPECT_EQ(queues.at(make_queue_key("us-west", "ranked", 1001)), 1);
    EXPECT_EQ(queues.at(make_queue_key("us-west", "ranked", 1999)), 2);
}

}  // namespace
}  // namespace matchmesh::core
