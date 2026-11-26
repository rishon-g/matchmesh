// Tests for the lobby ID prefix that tells one run of a node from another.

#include <gtest/gtest.h>

#include "matchmesh/node/identity.hpp"

namespace matchmesh::node {
namespace {

TEST(IdentityTest, LobbyIdPrefixUsesTheLow32BitsAsEightHexDigits) {
    EXPECT_EQ(lobby_id_prefix("node-c", 0x9f3c2a1e), "node-c-9f3c2a1e");
    // Zero-padded, and the high 32 bits are dropped.
    EXPECT_EQ(lobby_id_prefix("node-a", 0xabcdef0000000001ULL), "node-a-00000001");
}

TEST(IdentityTest, NewIncarnationIsNeverZero) {
    // Zero means "not using incarnations" to core::Membership.
    for (int i = 0; i < 1000; ++i) {
        EXPECT_NE(new_incarnation(), 0u);
    }
}

}  // namespace
}  // namespace matchmesh::node
