// Tests for HashRing: ownership, membership changes, and the distribution
// properties that make consistent hashing worth it.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/hash_ring.hpp"

namespace matchmesh::core {
namespace {

const std::vector<std::string> kFiveNodes = {"node-a", "node-b", "node-c", "node-d", "node-e"};
constexpr int kKeys = 10'000;

std::string key_name(int i) {
    return "queue-" + std::to_string(i);
}

HashRing five_node_ring(std::size_t virtual_nodes = HashRing::kDefaultVirtualNodes) {
    HashRing ring(virtual_nodes);
    for (const std::string& node : kFiveNodes) {
        ring.add_node(node);
    }
    return ring;
}

// How many of the test keys each node owns.
std::map<std::string, int> load_per_node(const HashRing& ring) {
    std::map<std::string, int> load;
    for (int i = 0; i < kKeys; ++i) {
        ++load[*ring.owner(key_name(i))];
    }
    return load;
}

// ---- stable_hash ----

TEST(StableHashTest, GivesTheSameValueEverywhere) {
    // Pinned values: if these ever change, nodes built from different
    // versions would disagree about who owns what.
    EXPECT_EQ(stable_hash(""), 0xefd01f60ba992926ULL);
    EXPECT_EQ(stable_hash("node-a#0"), stable_hash("node-a#0"));
}

TEST(StableHashTest, SimilarInputsLandFarApart) {
    EXPECT_NE(stable_hash("node-a#1"), stable_hash("node-a#2"));
    // A one-character change should flip roughly half of the 64 bits.
    const std::uint64_t diff = stable_hash("node-a#1") ^ stable_hash("node-a#2");
    EXPECT_GT(std::popcount(diff), 16);
}

// ---- Membership ----

TEST(HashRingTest, StartsEmptyAndOwnsNothing) {
    const HashRing ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.node_count(), 0u);
    EXPECT_FALSE(ring.owner("anything").has_value());
    EXPECT_TRUE(ring.owners("anything", 2).empty());
}

TEST(HashRingTest, AddAndRemoveNodes) {
    HashRing ring;
    EXPECT_TRUE(ring.add_node("node-a"));
    EXPECT_TRUE(ring.add_node("node-b"));
    EXPECT_FALSE(ring.add_node("node-a"));  // already there

    EXPECT_EQ(ring.node_count(), 2u);
    EXPECT_TRUE(ring.contains("node-a"));
    EXPECT_EQ(ring.nodes(), (std::vector<std::string>{"node-a", "node-b"}));

    EXPECT_TRUE(ring.remove_node("node-a"));
    EXPECT_FALSE(ring.remove_node("node-a"));  // already gone
    EXPECT_FALSE(ring.contains("node-a"));
    EXPECT_EQ(ring.node_count(), 1u);
}

TEST(HashRingTest, RejectsBadInput) {
    EXPECT_THROW(HashRing(0), std::invalid_argument);
    HashRing ring;
    EXPECT_THROW(ring.add_node(""), std::invalid_argument);
}

TEST(HashRingTest, ASingleNodeOwnsEverything) {
    HashRing ring;
    ring.add_node("node-a");
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(ring.owner(key_name(i)), "node-a");
    }
}

TEST(HashRingTest, RemovingTheLastNodeLeavesNoOwner) {
    HashRing ring;
    ring.add_node("node-a");
    ring.remove_node("node-a");
    EXPECT_FALSE(ring.owner("queue-1").has_value());
}

TEST(HashRingTest, KeysPastTheLastPointWrapAroundToTheFirst) {
    // Work out where every virtual node sits, using the same public hash the
    // ring uses, then find a key that lands after the very last point. Going
    // clockwise from there must wrap past 2^64-1 to the first point.
    const HashRing ring = five_node_ring();
    std::map<std::uint64_t, std::string> points;
    for (const std::string& node : kFiveNodes) {
        for (std::size_t i = 0; i < HashRing::kDefaultVirtualNodes; ++i) {
            points.emplace(stable_hash(node + "#" + std::to_string(i)), node);
        }
    }
    const std::uint64_t last_point = points.rbegin()->first;
    const std::string& first_node = points.begin()->second;

    int found = 0;
    for (int i = 0; i < 200'000 && found < 3; ++i) {
        const std::string key = "wrap-" + std::to_string(i);
        if (stable_hash(key) > last_point) {
            ++found;
            EXPECT_EQ(ring.owner(key), first_node) << key;
        }
    }
    ASSERT_GT(found, 0) << "no test key landed past the last point";
}

// ---- Every node must compute the same ring ----

TEST(HashRingTest, OwnershipDoesNotDependOnJoinOrder) {
    HashRing forwards;
    for (const std::string& node : kFiveNodes) {
        forwards.add_node(node);
    }
    HashRing backwards;
    for (auto it = kFiveNodes.rbegin(); it != kFiveNodes.rend(); ++it) {
        backwards.add_node(*it);
    }
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(forwards.owner(key_name(i)), backwards.owner(key_name(i))) << key_name(i);
    }
}

TEST(HashRingTest, LeavingAndRejoiningRestoresTheSameRing) {
    const HashRing original = five_node_ring();
    HashRing ring = five_node_ring();
    ring.remove_node("node-c");
    ring.add_node("node-c");
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(ring.owner(key_name(i)), original.owner(key_name(i))) << key_name(i);
    }
}

// ---- Owners (primary + backups) ----

TEST(HashRingTest, OwnersAreDistinctAndStartWithTheOwner) {
    const HashRing ring = five_node_ring();
    for (int i = 0; i < 200; ++i) {
        const std::vector<std::string> owners = ring.owners(key_name(i), 3);
        ASSERT_EQ(owners.size(), 3u);
        EXPECT_EQ(owners[0], ring.owner(key_name(i)));
        EXPECT_EQ(std::set<std::string>(owners.begin(), owners.end()).size(), 3u)
            << "the same node appears twice for " << key_name(i);
    }
}

TEST(HashRingTest, OwnersNeverReturnsMoreNodesThanExist) {
    HashRing ring;
    ring.add_node("node-a");
    ring.add_node("node-b");
    EXPECT_EQ(ring.owners("queue-1", 5).size(), 2u);
    EXPECT_TRUE(ring.owners("queue-1", 0).empty());
}

TEST(HashRingTest, BackupTakesOverWhenTheOwnerLeaves) {
    // The second owner is exactly who inherits the key if the first leaves.
    // That's what lets a backup replica take over in Phase 5.
    const HashRing before = five_node_ring();
    for (int i = 0; i < 500; ++i) {
        const std::vector<std::string> owners = before.owners(key_name(i), 2);
        HashRing after = before;
        after.remove_node(owners[0]);
        EXPECT_EQ(after.owner(key_name(i)), owners[1]) << key_name(i);
    }
}

TEST(HashRingTest, QueueKeysUseTheirStringForm) {
    const HashRing ring = five_node_ring();
    const QueueKey key = make_queue_key("us-west", "ranked", 1300);
    EXPECT_EQ(ring.owner(key), ring.owner(to_string(key)));
    EXPECT_EQ(ring.owners(key, 2), ring.owners(to_string(key), 2));
}

// ---- Distribution (the plan's Phase 4 test) ----

TEST(HashRingDistributionTest, KeysAreSpreadEvenlyAcrossFiveNodes) {
    const std::map<std::string, int> load = load_per_node(five_node_ring());

    ASSERT_EQ(load.size(), kFiveNodes.size()) << "some node owns nothing";
    const int ideal = kKeys / static_cast<int>(kFiveNodes.size());  // 2,000
    for (const auto& [node, count] : load) {
        // Measured: every node lands within about 14% of ideal. Allow 20%.
        EXPECT_GT(count, ideal * 8 / 10) << node << " owns too few keys";
        EXPECT_LT(count, ideal * 12 / 10) << node << " owns too many keys";
    }
}

TEST(HashRingDistributionTest, VirtualNodesAreWhatMakeItEven) {
    // With one point per node the arcs are random sizes, so load is lopsided.
    const auto spread = [](const std::map<std::string, int>& load) {
        int low = kKeys;
        int high = 0;
        for (const auto& [node, count] : load) {
            low = std::min(low, count);
            high = std::max(high, count);
        }
        return static_cast<double>(high) / low;
    };
    const double one = spread(load_per_node(five_node_ring(1)));
    const double hundred = spread(load_per_node(five_node_ring(100)));

    EXPECT_GT(one, 2.0) << "1 virtual node: busiest/quietest node";
    EXPECT_LT(hundred, 1.5) << "100 virtual nodes: busiest/quietest node";
}

TEST(HashRingDistributionTest, RemovingANodeMovesOnlyItsOwnKeys) {
    const HashRing before = five_node_ring();
    HashRing after = before;
    after.remove_node("node-c");

    int moved = 0;
    for (int i = 0; i < kKeys; ++i) {
        const std::string was = *before.owner(key_name(i));
        const std::string now = *after.owner(key_name(i));
        if (was != now) {
            ++moved;
            // Only node-c's keys may move; everyone else's stay put.
            EXPECT_EQ(was, "node-c") << key_name(i) << " moved from " << was;
        }
    }
    // node-c owned about 1/5 of the keys, so about 1/5 move (measured ~17%).
    const double fraction = static_cast<double>(moved) / kKeys;
    EXPECT_GT(fraction, 0.12);
    EXPECT_LT(fraction, 0.28);
}

TEST(HashRingDistributionTest, AddingANodeOnlyTakesKeysForItself) {
    const HashRing before = five_node_ring();
    HashRing after = before;
    after.add_node("node-f");

    int moved = 0;
    for (int i = 0; i < kKeys; ++i) {
        const std::string was = *before.owner(key_name(i));
        const std::string now = *after.owner(key_name(i));
        if (was != now) {
            ++moved;
            // Keys only ever move *to* the new node, never between old ones.
            EXPECT_EQ(now, "node-f") << key_name(i) << " moved to " << now;
        }
    }
    // The new node should take roughly its fair share, 1/6.
    const double fraction = static_cast<double>(moved) / kKeys;
    EXPECT_GT(fraction, 0.10);
    EXPECT_LT(fraction, 0.24);
}

TEST(HashRingDistributionTest, ModuloHashingWouldMoveAlmostEverything) {
    // For contrast: the naive `hash % node_count` approach. Going from 5 to 4
    // nodes reassigns about 80% of keys, which is why we use a ring instead.
    int moved = 0;
    for (int i = 0; i < kKeys; ++i) {
        const std::uint64_t h = stable_hash(key_name(i));
        moved += (h % 5) != (h % 4) ? 1 : 0;
    }
    EXPECT_GT(static_cast<double>(moved) / kKeys, 0.7);
}

}  // namespace
}  // namespace matchmesh::core
