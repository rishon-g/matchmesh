cmake_minimum_required(VERSION 3.25)

project(MatchMesh
    VERSION 0.1.0
    DESCRIPTION "Fault-tolerant distributed matchmaking cluster"
    LANGUAGES CXX)

# ---- Language standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # fail instead of silently using an older standard
set(CMAKE_CXX_EXTENSIONS OFF)         # use -std=c++20, not GNU's -std=gnu++20

# Write compile_commands.json so editors (VS Code, CLion, clangd) understand the code.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Shared warning flags, applied per target below ----
add_library(matchmesh_warnings INTERFACE)
target_compile_options(matchmesh_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# ---- Sub-projects ----
add_subdirectory(core)
add_subdirectory(proto)   # generated gRPC/Protobuf code
add_subdirectory(node)
add_subdirectory(loadgen)

# ---- Tests ----
# On by default; pass -DMATCHMESH_BUILD_TESTS=OFF to skip them.
option(MATCHMESH_BUILD_TESTS "Build the GoogleTest suites" ON)
if(MATCHMESH_BUILD_TESTS)
    enable_testing()          # lets ctest find the tests registered below
    add_subdirectory(tests)
endif()
