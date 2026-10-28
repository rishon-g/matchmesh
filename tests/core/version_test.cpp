// Tests for version(): it must be a plain MAJOR.MINOR.PATCH string, since
// the node and load generator print it in their startup banners.

#include <regex>
#include <string>

#include <gtest/gtest.h>

#include "matchmesh/core/version.hpp"

namespace matchmesh::core {
namespace {

TEST(VersionTest, IsMajorMinorPatch) {
    const std::string v(version());
    EXPECT_TRUE(std::regex_match(v, std::regex(R"(\d+\.\d+\.\d+)"))) << v;
}

}  // namespace
}  // namespace matchmesh::core
