// Tests for place(): the primary is the ring's owner, the backup is the next
// distinct node, and a one-node cluster has no backup.

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/placement.hpp"

namespace matchmesh::core {
namespace {

const QueueKey kQueue{"us-west", "ranked", 3};

TEST(PlacementTest, OneNodeClusterHasNoBackup) {
    HashRing ring;
    ring.add_node("node-a");

    const Placement placement = place(ring, 7, kQueue);

    EXPECT_EQ("node-a", placement.primary);
    EXPECT_FALSE(placement.backup.has_value());
    EXPECT_EQ(7u, placement.epoch);
}

TEST(PlacementTest, EmptyRingIsRejected) {
    EXPECT_THROW(static_cast<void>(place(HashRing{}, 1, kQueue)), std::invalid_argument);
}

TEST(PlacementTest, BackupIsADifferentNodeFromThePrimary) {
    HashRing ring;
    ring.add_node("node-a");
    ring.add_node("node-b");
    ring.add_node("node-c");

    const Placement placement = place(ring, 1, kQueue);

    EXPECT_EQ(ring.owner(kQueue), placement.primary);
    ASSERT_TRUE(placement.backup.has_value());
    EXPECT_NE(placement.primary, *placement.backup);
}

TEST(PlacementTest, BackupIsTheNextDistinctOwnerOnTheRing) {
    HashRing ring;
    ring.add_node("node-a");
    ring.add_node("node-b");
    ring.add_node("node-c");

    const auto owners = ring.owners(kQueue, 2);
    ASSERT_EQ(2u, owners.size());

    const Placement placement = place(ring, 1, kQueue);

    EXPECT_EQ(owners[0], placement.primary);
    EXPECT_EQ(owners[1], placement.backup);
}

}  // namespace
}  // namespace matchmesh::core
