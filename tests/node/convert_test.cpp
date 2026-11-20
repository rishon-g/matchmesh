// Tests for the core <-> protobuf translations in convert.hpp.

#include <gtest/gtest.h>

#include "matchmesh/node/convert.hpp"

namespace matchmesh::node {
namespace {

TEST(ConvertTest, ChangeFromProtoRejectsSnapshotsAndMissingOps) {
    v1::ReplicateRequest request;
    EXPECT_FALSE(change_from_proto(request).has_value());  // no op set

    request.mutable_snapshot();
    EXPECT_FALSE(change_from_proto(request).has_value());
}

}  // namespace
}  // namespace matchmesh::node
