# node: the server process.

# The node's gRPC services, as a library so the tests can start them in-process
# without running the whole program.
add_library(matchmesh_node_lib
    src/convert.cpp
    src/cluster_service.cpp
    src/matchmaking_service.cpp
    src/router.cpp)
target_include_directories(matchmesh_node_lib PUBLIC include)
# PUBLIC: code that uses these services also sees core's and proto's types.
target_link_libraries(matchmesh_node_lib
    PUBLIC matchmesh_core matchmesh_proto
    PRIVATE matchmesh_warnings)

# The executable: just argument parsing and server startup.
add_executable(matchmesh_node src/main.cpp)
target_link_libraries(matchmesh_node PRIVATE matchmesh_node_lib matchmesh_warnings)

# A small command-line client for trying the cluster by hand.
add_executable(matchmesh_cli src/cli_main.cpp)
target_link_libraries(matchmesh_cli PRIVATE matchmesh_node_lib matchmesh_warnings)
