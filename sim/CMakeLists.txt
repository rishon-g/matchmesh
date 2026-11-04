# sim: a whole cluster in one process (simulated network and clock) over the
# real core, for the in-browser demo. Builds natively for the tests and with
# Emscripten for the browser.
add_library(matchmesh_sim src/cluster.cpp)
target_include_directories(matchmesh_sim PUBLIC include)
target_link_libraries(matchmesh_sim PUBLIC matchmesh_core PRIVATE matchmesh_warnings)
