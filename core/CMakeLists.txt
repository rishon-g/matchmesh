# core: the pure matchmaking library. No networking lives here.
add_library(matchmesh_core
    src/version.cpp
    src/types.cpp
    src/skill_queue.cpp
    src/matcher.cpp
    src/engine.cpp
    src/hash_ring.cpp
    src/membership.cpp
    src/placement.cpp
    src/shard_coordinator.cpp)

# PUBLIC: anyone who links matchmesh_core can also #include its headers.
target_include_directories(matchmesh_core PUBLIC include)

# version() reports the version set in the top-level project() call, so it
# never drifts from the CMake project.
set_source_files_properties(src/version.cpp PROPERTIES
    COMPILE_DEFINITIONS "MATCHMESH_VERSION=\"${PROJECT_VERSION}\"")

# The engine runs worker threads. Threads::Threads adds whatever flags the
# platform needs for that (-pthread on Linux). Not in the WebAssembly demo:
# it runs the engine with no workers, and threaded WebAssembly would need
# cross-origin isolation headers that GitHub Pages can't send.
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(matchmesh_core PUBLIC Threads::Threads)
else()
    # Emscripten's libc++ still files std::jthread and std::stop_token under
    # "experimental"; this unlocks them (they're never started there).
    target_compile_options(matchmesh_core PUBLIC -fexperimental-library)
    target_link_options(matchmesh_core PUBLIC -fexperimental-library)
endif()

# PRIVATE: warnings apply when compiling core itself, not to its users.
target_link_libraries(matchmesh_core PRIVATE matchmesh_warnings)
