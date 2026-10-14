// Tests for peer-list parsing and the Router.

#include <map>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "matchmesh/node/router.hpp"

namespace matchmesh::node {
namespace {

TEST(ParsePeersTest, ParsesAListOfIdAddressPairs) {
    const auto peers = parse_peers("node-a=node-a:50051,node-b=10.0.0.2:50051");
    EXPECT_EQ(peers, (std::map<std::string, std::string>{{"node-a", "node-a:50051"},
                                                        {"node-b", "10.0.0.2:50051"}}));
}

TEST(ParsePeersTest, ToleratesSpacesAndEmptyEntries) {
    const auto peers = parse_peers(" node-a = a:1 , ,node-b=b:2, ");
    EXPECT_EQ(peers, (std::map<std::string, std::string>{{"node-a", "a:1"}, {"node-b", "b:2"}}));
    EXPECT_TRUE(parse_peers("").empty());
}

TEST(ParsePeersTest, RejectsMalformedEntries) {
    EXPECT_THROW((void)parse_peers("node-a"), std::invalid_argument);             // no '='
    EXPECT_THROW((void)parse_peers("=a:1"), std::invalid_argument);               // no id
    EXPECT_THROW((void)parse_peers("node-a="), std::invalid_argument);            // no address
    EXPECT_THROW((void)parse_peers("node-a=a:1,node-a=b:2"), std::invalid_argument);  // twice
}

TEST(RouterTest, ASingleNodeOwnsEveryQueue) {
    const Router router("node-a", {});
    EXPECT_EQ(router.members().size(), 1u);
    EXPECT_TRUE(router.is_local(core::make_queue_key("us-west", "ranked", 1300)));
    EXPECT_TRUE(router.is_local(core::make_queue_key("eu", "casual", 800)));
}

TEST(RouterTest, IgnoresItselfInThePeerList) {
    // Every node can be given the same full list.
    const Router router("node-a", parse_peers("node-a=a:1,node-b=b:1,node-c=c:1"));
    EXPECT_EQ(router.members().size(), 3u);
    EXPECT_EQ(router.peers().size(), 2u);
    EXPECT_FALSE(router.peers().contains("node-a"));
}

TEST(RouterTest, EveryNodeAgreesOnWhoOwnsEachQueue) {
    const auto peers = parse_peers("node-a=a:1,node-b=b:1,node-c=c:1,node-d=d:1,node-e=e:1");
    const Router a("node-a", peers);
    const Router c("node-c", peers);
    const Router e("node-e", peers);
    for (int i = 0; i < 1000; ++i) {
        const auto queue = core::make_queue_key("region-" + std::to_string(i), "ranked", 1300);
        EXPECT_EQ(a.owner_of(queue), c.owner_of(queue));
        EXPECT_EQ(a.owner_of(queue), e.owner_of(queue));
    }
}

TEST(RouterTest, HasAStubForEveryPeerButNotItself) {
    const Router router("node-a", parse_peers("node-b=b:1"));
    EXPECT_NO_THROW((void)router.stub_for("node-b"));
    EXPECT_THROW((void)router.stub_for("node-a"), std::out_of_range);
    EXPECT_THROW((void)router.stub_for("node-z"), std::out_of_range);
}

TEST(RouterTest, RejectsAnEmptyId) {
    EXPECT_THROW(Router("", {}), std::invalid_argument);
}

}  // namespace
}  // namespace matchmesh::node
