# tests: GoogleTest suites. Run with `ctest --preset debug`.

# Ubuntu's libgtest-dev ships a working CMake config, so (unlike gRPC)
# find_package works here. It provides the GTest::gtest_main target.
find_package(GTest REQUIRED)
include(GoogleTest)  # for gtest_discover_tests

add_executable(matchmesh_core_tests
    core/types_test.cpp
    core/skill_queue_test.cpp
    core/matcher_test.cpp
    core/engine_test.cpp
    core/replication_test.cpp
    core/hash_ring_test.cpp
    core/membership_test.cpp)

# gtest_main supplies main(), so the test files contain only tests.
target_link_libraries(matchmesh_core_tests
    PRIVATE matchmesh_core GTest::gtest_main matchmesh_warnings)

# Registers every TEST() with ctest individually, so a failure report
# names the exact test that broke.
gtest_discover_tests(matchmesh_core_tests)

# ---- Load generator: histogram and workload (no networking) ----
add_executable(matchmesh_loadgen_tests
    loadgen/histogram_test.cpp
    loadgen/workload_test.cpp)
target_link_libraries(matchmesh_loadgen_tests
    PRIVATE matchmesh_loadgen_lib GTest::gtest_main matchmesh_warnings)
gtest_discover_tests(matchmesh_loadgen_tests)

# ---- Node service tests (real gRPC server, in-process) ----
# Skipped in the tsan build: the gRPC packages from apt aren't compiled with
# ThreadSanitizer, so TSan reports false races inside gRPC itself. TSan runs
# on core (above), where every line is ours.
if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
    add_executable(matchmesh_node_tests
        node/matchmaking_service_test.cpp
        node/router_test.cpp
        node/forwarding_test.cpp
        node/cluster_test.cpp
        node/shard_manager_test.cpp
        node/loadgen_runner_test.cpp)
    target_link_libraries(matchmesh_node_tests
        PRIVATE matchmesh_node_lib matchmesh_loadgen_runner GTest::gtest_main
                matchmesh_warnings)
    gtest_discover_tests(matchmesh_node_tests)
endif()

# ---- Stress tests (many threads at once; meant for the tsan preset) ----
# Core only, so ThreadSanitizer checks every line. Labelled "stress" so they
# can be run on their own: ctest --preset tsan -L stress
add_executable(matchmesh_stress_tests
    stress/engine_stress_test.cpp)
target_link_libraries(matchmesh_stress_tests
    PRIVATE matchmesh_core GTest::gtest_main matchmesh_warnings)
gtest_discover_tests(matchmesh_stress_tests PROPERTIES LABELS stress)
