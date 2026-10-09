# tests: GoogleTest unit suites. Run with `ctest --preset debug`.

# Ubuntu's libgtest-dev ships a working CMake config, so (unlike gRPC)
# find_package works here. It provides the GTest::gtest_main target.
find_package(GTest REQUIRED)
include(GoogleTest)  # for gtest_discover_tests

add_executable(matchmesh_core_tests
    core/types_test.cpp
    core/skill_queue_test.cpp
    core/matcher_test.cpp)

# gtest_main supplies main(), so the test files contain only tests.
target_link_libraries(matchmesh_core_tests
    PRIVATE matchmesh_core GTest::gtest_main matchmesh_warnings)

# Registers every TEST() with ctest individually, so a failure report
# names the exact test that broke.
gtest_discover_tests(matchmesh_core_tests)
