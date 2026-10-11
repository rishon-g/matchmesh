# core: the pure matchmaking library. No networking lives here.
add_library(matchmesh_core
    src/version.cpp
    src/types.cpp
    src/skill_queue.cpp
    src/matcher.cpp
    src/engine.cpp)

# PUBLIC: anyone who links matchmesh_core can also #include its headers.
target_include_directories(matchmesh_core PUBLIC include)

# The engine runs worker threads. Threads::Threads adds whatever flags the
# platform needs for that (-pthread on Linux).
find_package(Threads REQUIRED)
target_link_libraries(matchmesh_core PUBLIC Threads::Threads)

# PRIVATE: warnings apply when compiling core itself, not to its users.
target_link_libraries(matchmesh_core PRIVATE matchmesh_warnings)
