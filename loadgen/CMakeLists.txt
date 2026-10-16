# loadgen: the 10,000-player load generator.

# Histogram and workload: plain C++, no networking, so they're tested on
# their own (including under ThreadSanitizer).
add_library(matchmesh_loadgen_lib
    src/histogram.cpp
    src/workload.cpp)
target_include_directories(matchmesh_loadgen_lib PUBLIC include)
target_link_libraries(matchmesh_loadgen_lib PRIVATE matchmesh_warnings)

# The runner talks gRPC to the cluster.
add_library(matchmesh_loadgen_runner src/runner.cpp)
target_link_libraries(matchmesh_loadgen_runner
    PUBLIC matchmesh_loadgen_lib matchmesh_node_lib
    PRIVATE matchmesh_warnings)

add_executable(matchmesh_loadgen src/main.cpp)
target_link_libraries(matchmesh_loadgen PRIVATE matchmesh_loadgen_runner matchmesh_warnings)
