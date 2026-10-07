# core: the pure matchmaking library. No networking lives here.
add_library(matchmesh_core
    src/version.cpp
    src/types.cpp
    src/skill_queue.cpp
    src/matcher.cpp)

# PUBLIC: anyone who links matchmesh_core can also #include its headers.
target_include_directories(matchmesh_core PUBLIC include)

# PRIVATE: warnings apply when compiling core itself, not to its users.
target_link_libraries(matchmesh_core PRIVATE matchmesh_warnings)
