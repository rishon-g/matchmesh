# gateway: the bridge between the browser dashboard and the cluster.
# Browsers can't speak gRPC, so this polls every node's GetTopology and pushes
# one merged JSON picture of the cluster over a WebSocket.

include(FetchContent)
find_package(Threads REQUIRED)

# Crow (a small header-only C++ web framework) and the standalone Asio it is
# built on. Pinned to exact releases so every build uses the same code. Only
# downloaded, never built (SOURCE_SUBDIR points nowhere), since both are
# header-only and we just need their include directories.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-38-2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_Declare(crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(asio crow)

add_library(matchmesh_crow INTERFACE)
# SYSTEM: their headers don't follow our warning flags, and that's not ours to fix.
target_include_directories(matchmesh_crow SYSTEM INTERFACE
    ${crow_SOURCE_DIR}/include
    ${asio_SOURCE_DIR}/asio/include)
target_link_libraries(matchmesh_crow INTERFACE Threads::Threads)

# Merging node answers into JSON, and polling the nodes: a library so the
# tests can use them directly.
add_library(matchmesh_gateway_lib
    src/snapshot.cpp
    src/poller.cpp)
target_include_directories(matchmesh_gateway_lib PUBLIC include)
target_link_libraries(matchmesh_gateway_lib
    PUBLIC matchmesh_node_lib matchmesh_crow
    PRIVATE matchmesh_warnings)

add_executable(matchmesh_gateway src/main.cpp)
target_link_libraries(matchmesh_gateway PRIVATE matchmesh_gateway_lib matchmesh_warnings)
