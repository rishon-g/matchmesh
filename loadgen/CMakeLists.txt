# loadgen: the 10,000-player simulator (built in Phase 6).
add_executable(matchmesh_loadgen src/main.cpp)
target_link_libraries(matchmesh_loadgen PRIVATE matchmesh_core matchmesh_warnings)
