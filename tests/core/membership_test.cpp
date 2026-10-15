// Tests for Membership: failure detection, the coordinator rule and epochs.
// Time is passed in explicitly, so every scenario is exact and instant.

#include <atomic>
#include <chrono>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "matchmesh/core/membership.hpp"

namespace matchmesh::core {
namespace {

using namespace std::chrono_literals;
using Names = std::vector<std::string>;

const Names kNodes = {"node-a", "node-b", "node-c", "node-d", "node-e"};

// Records a heartbeat from every listed node at time `now`.
void hear_from(Membership& m, const Names& nodes, TimePoint now) {
    for (const std::string& node : nodes) {
        m.record_contact(node, now);
    }
}

// ---- Views and their ordering ----

TEST(MembershipViewTest, CoordinatorIsTheLowestMember) {
    EXPECT_EQ((MembershipView{3, {"node-b", "node-c"}}.coordinator()), "node-b");
    EXPECT_EQ(MembershipView{}.coordinator(), "");
}

TEST(MembershipViewTest, HigherEpochIsNewer) {
    const MembershipView old_view{3, {"node-a", "node-b"}};
    const MembershipView new_view{4, {"node-b"}};
    EXPECT_TRUE(is_newer(new_view, old_view));
    EXPECT_FALSE(is_newer(old_view, new_view));
    EXPECT_FALSE(is_newer(old_view, old_view));  // the same view is not "newer"
}

TEST(MembershipViewTest, EqualEpochsResolveToTheLowerCoordinator) {
    // Two halves of a split cluster each published epoch 5. Every node must
    // still pick the same one when they meet: the lower coordinator's.
    const MembershipView left{5, {"node-a", "node-b"}};
    const MembershipView right{5, {"node-c", "node-d", "node-e"}};
    EXPECT_TRUE(is_newer(left, right));
    EXPECT_FALSE(is_newer(right, left));
}

// ---- Starting up ----

TEST(MembershipTest, StartsWithEveryoneInEpochZero) {
    const Membership m("node-c", kNodes, TimePoint{});
    EXPECT_EQ(m.view(), (MembershipView{0, kNodes}));
    EXPECT_EQ(m.alive(TimePoint{}), kNodes);
    EXPECT_EQ(m.local_coordinator(TimePoint{}), "node-a");
}

TEST(MembershipTest, RejectsASelfThatIsNotInTheCluster) {
    EXPECT_THROW(Membership("node-z", kNodes, TimePoint{}), std::invalid_argument);
    EXPECT_THROW(Membership("", kNodes, TimePoint{}), std::invalid_argument);
}

// ---- Failure detection ----

TEST(MembershipTest, ANodeIsAliveWhileWeKeepHearingFromIt) {
    Membership m("node-a", kNodes, TimePoint{});
    for (int i = 1; i <= 10; ++i) {
        hear_from(m, {"node-b"}, TimePoint{} + i * 500ms);  // every 500 ms
    }
    EXPECT_TRUE(m.is_alive("node-b", TimePoint{} + 5s + 1s));
}

TEST(MembershipTest, ANodeSilentForMoreThanOnePointFiveSecondsIsSuspected) {
    Membership m("node-a", kNodes, TimePoint{});
    hear_from(m, {"node-b"}, TimePoint{} + 1s);

    EXPECT_TRUE(m.is_alive("node-b", TimePoint{} + 2500ms));   // exactly 1.5 s of silence
    EXPECT_FALSE(m.is_alive("node-b", TimePoint{} + 2501ms));  // just over
}

TEST(MembershipTest, WeAlwaysConsiderOurselvesAlive) {
    const Membership m("node-c", kNodes, TimePoint{});
    EXPECT_TRUE(m.is_alive("node-c", TimePoint{} + 1h));
    EXPECT_EQ(m.alive(TimePoint{} + 1h), (Names{"node-c"}));
}

TEST(MembershipTest, StrangersAndOutOfOrderContactsAreHandled) {
    Membership m("node-a", kNodes, TimePoint{});
    m.record_contact("node-z", TimePoint{} + 1s);  // not in the cluster: ignored
    EXPECT_FALSE(m.is_alive("node-z", TimePoint{} + 1s));

    // A late-arriving older contact must not move "last heard" backwards.
    m.record_contact("node-b", TimePoint{} + 3s);
    m.record_contact("node-b", TimePoint{} + 2s);
    EXPECT_TRUE(m.is_alive("node-b", TimePoint{} + 4500ms));
}

// ---- The coordinator publishes views ----

TEST(MembershipTest, CoordinatorPublishesANewEpochWhenANodeDies) {
    Membership a("node-a", kNodes, TimePoint{});
    const TimePoint t = TimePoint{} + 2s;
    hear_from(a, {"node-b", "node-c", "node-e"}, t);  // node-d has gone quiet

    const auto published = a.tick(t);

    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(*published, (MembershipView{1, {"node-a", "node-b", "node-c", "node-e"}}));
    EXPECT_EQ(a.view(), *published);
}

TEST(MembershipTest, NothingIsPublishedWhenNothingChanged) {
    Membership a("node-a", kNodes, TimePoint{});
    hear_from(a, {"node-b", "node-c", "node-d", "node-e"}, TimePoint{} + 1s);
    EXPECT_FALSE(a.tick(TimePoint{} + 1s).has_value());
    EXPECT_EQ(a.view().epoch, 0u);
}

TEST(MembershipTest, NonCoordinatorsNeverPublish) {
    // node-c also sees node-d die, but node-a (lower ID) is alive, so it is
    // node-a's job to publish. node-c waits for node-a's view.
    Membership c("node-c", kNodes, TimePoint{});
    const TimePoint t = TimePoint{} + 2s;
    hear_from(c, {"node-a", "node-b", "node-e"}, t);

    EXPECT_FALSE(c.tick(t).has_value());
    EXPECT_EQ(c.view().epoch, 0u);
}

TEST(MembershipTest, EpochsKeepIncreasingWithEveryChange) {
    Membership a("node-a", kNodes, TimePoint{});
    hear_from(a, {"node-b", "node-c", "node-e"}, TimePoint{} + 2s);
    ASSERT_EQ(a.tick(TimePoint{} + 2s)->epoch, 1u);  // node-d out

    hear_from(a, {"node-b", "node-c", "node-d", "node-e"}, TimePoint{} + 3s);
    const auto back = a.tick(TimePoint{} + 3s);  // node-d back
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, (MembershipView{2, kNodes}));
}

TEST(MembershipTest, WhenTheCoordinatorDiesTheNextLowestTakesOver) {
    // node-a dies. node-b is now the lowest alive node, so it publishes,
    // continuing from the epoch it had already adopted from node-a.
    Membership b("node-b", kNodes, TimePoint{});
    ASSERT_TRUE(b.observe(MembershipView{7, kNodes}));

    const TimePoint t = TimePoint{} + 2s;
    hear_from(b, {"node-c", "node-d", "node-e"}, t);  // node-a has gone quiet

    EXPECT_EQ(b.local_coordinator(t), "node-b");
    const auto published = b.tick(t);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(*published, (MembershipView{8, {"node-b", "node-c", "node-d", "node-e"}}));
    EXPECT_EQ(published->coordinator(), "node-b");
}

// ---- Spreading views ----

TEST(MembershipTest, NodesAdoptNewerViewsAndIgnoreOlderOnes) {
    Membership c("node-c", kNodes, TimePoint{});
    const MembershipView v3{3, {"node-a", "node-b", "node-c"}};

    EXPECT_TRUE(c.observe(v3));
    EXPECT_EQ(c.view(), v3);
    EXPECT_FALSE(c.observe(MembershipView{2, kNodes}));  // stale: ignored
    EXPECT_FALSE(c.observe(v3));                          // already have it
    EXPECT_EQ(c.view(), v3);
}

TEST(MembershipTest, EmptyViewsAreNeverAdopted) {
    // A bare ping carries epoch 0 and no members. Its empty coordinator would
    // sort before "node-a", so without a guard it would win the tie-break.
    Membership c("node-c", kNodes, TimePoint{});
    EXPECT_FALSE(c.observe(MembershipView{0, {}}));
    EXPECT_FALSE(c.observe(MembershipView{99, {}}));
    EXPECT_EQ(c.view(), (MembershipView{0, kNodes}));
}

TEST(MembershipTest, ANodeDeclaredDeadRejoinsItself) {
    // node-a was slow, so node-b published a view without it. When node-a
    // learns that view, it is still the lowest alive node, so it publishes
    // a newer view that puts itself back.
    Membership a("node-a", kNodes, TimePoint{});
    hear_from(a, {"node-b", "node-c", "node-d", "node-e"}, TimePoint{} + 1s);
    ASSERT_TRUE(a.observe(MembershipView{4, {"node-b", "node-c", "node-d", "node-e"}}));

    const auto rejoined = a.tick(TimePoint{} + 1s);
    ASSERT_TRUE(rejoined.has_value());
    EXPECT_EQ(*rejoined, (MembershipView{5, kNodes}));
}

TEST(MembershipTest, TwoNodesConvergeThroughHeartbeats) {
    // node-a (coordinator) and node-c, exchanging views the way heartbeats do.
    Membership a("node-a", kNodes, TimePoint{});
    Membership c("node-c", kNodes, TimePoint{});
    const TimePoint t = TimePoint{} + 2s;
    hear_from(a, {"node-b", "node-c", "node-e"}, t);
    hear_from(c, {"node-a", "node-b", "node-e"}, t);

    EXPECT_FALSE(c.tick(t).has_value());  // not c's job
    const auto published = a.tick(t);
    ASSERT_TRUE(published.has_value());
    EXPECT_TRUE(c.observe(a.view()));  // a heartbeat carries a's view to c

    EXPECT_EQ(c.view(), a.view());
    EXPECT_EQ(c.view().members, (Names{"node-a", "node-b", "node-c", "node-e"}));
}

// ---- Thread safety (meant for the tsan preset) ----

// ---- Incarnations: catching restarts ----

TEST(MembershipTest, TheCoordinatorWaitsToHearFromEveryoneBeforePublishing) {
    Membership a("node-a", {"node-a", "node-b", "node-c"}, TimePoint{}, {}, 11);
    // Just started: b and c count as alive (grace period) but are unheard.
    a.record_contact("node-b", TimePoint{} + 100ms, 22);
    EXPECT_FALSE(a.tick(TimePoint{} + 100ms).has_value());

    a.record_contact("node-c", TimePoint{} + 200ms, 33);
    const auto published = a.tick(TimePoint{} + 200ms);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->epoch, 1u);
    EXPECT_EQ(published->incarnations,
              (std::map<std::string, std::uint64_t>{{"node-a", 11}, {"node-b", 22}, {"node-c", 33}}));
    EXPECT_TRUE(published->lists("node-b", 22));
    EXPECT_FALSE(published->lists("node-b", 23));
}

TEST(MembershipTest, ANodeThatRestartsBeforeAnyoneNoticesGetsANewEpoch) {
    Membership a("node-a", {"node-a", "node-b"}, TimePoint{}, {}, 11);
    a.record_contact("node-b", TimePoint{} + 100ms, 22);
    ASSERT_EQ(a.tick(TimePoint{} + 100ms)->epoch, 1u);

    // b crashes and is back 200 ms later: never silent long enough to be
    // suspected, but it's a new process with empty memory.
    a.record_contact("node-b", TimePoint{} + 300ms, 23);
    const auto published = a.tick(TimePoint{} + 300ms);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->epoch, 2u);
    EXPECT_EQ(published->members, (Names{"node-a", "node-b"}));
    EXPECT_TRUE(published->lists("node-b", 23));
}

TEST(MembershipTest, ARestartedCoordinatorRepublishesItself) {
    // The cluster's view lists node-a's previous incarnation (11).
    const MembershipView cluster_view{4, {"node-a", "node-b"}, {{"node-a", 11}, {"node-b", 22}}};
    Membership a("node-a", {"node-a", "node-b"}, TimePoint{}, {}, 12);  // restarted
    ASSERT_TRUE(a.observe(cluster_view));
    EXPECT_FALSE(a.view().lists("node-a", 12));  // not really a member yet

    a.record_contact("node-b", TimePoint{} + 50ms, 22);
    const auto published = a.tick(TimePoint{} + 50ms);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->epoch, 5u);
    EXPECT_TRUE(published->lists("node-a", 12));
}

// ---- Concurrency ----

TEST(MembershipTest, SafeToUseFromManyThreadsAtOnce) {
    Membership a("node-a", kNodes, Clock::now());
    std::atomic<bool> done{false};
    std::vector<std::jthread> threads;
    for (const char* peer : {"node-b", "node-c", "node-d"}) {
        threads.emplace_back([&a, &done, peer] {
            while (!done) {
                a.record_contact(peer, Clock::now());
            }
        });
    }
    threads.emplace_back([&a, &done] {
        while (!done) {
            (void)a.tick(Clock::now());
            (void)a.alive(Clock::now());
        }
    });
    threads.emplace_back([&a, &done] {
        for (std::uint64_t epoch = 1; !done; ++epoch) {
            (void)a.observe(MembershipView{epoch, {"node-a", "node-b"}});
            (void)a.view();
        }
    });
    std::this_thread::sleep_for(100ms);
    done = true;
    threads.clear();
    EXPECT_GE(a.view().epoch, 1u);
}

}  // namespace
}  // namespace matchmesh::core
