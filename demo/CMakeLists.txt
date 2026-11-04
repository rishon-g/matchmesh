# demo: the simulated cluster compiled to WebAssembly for the dashboard's
# demo mode. Configured only by an Emscripten build:
#
#   emcmake cmake --preset wasm && cmake --build --preset wasm
#
# produces build/wasm/demo/matchmesh-sim.mjs, a single ES module with the
# WebAssembly inlined, which the dashboard loads at runtime.
add_executable(matchmesh_sim_wasm bindings.cpp)
target_link_libraries(matchmesh_sim_wasm PRIVATE matchmesh_sim matchmesh_warnings)
set_target_properties(matchmesh_sim_wasm PROPERTIES
    OUTPUT_NAME matchmesh-sim
    SUFFIX .mjs)
target_link_options(matchmesh_sim_wasm PRIVATE
    -lembind
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createMatchMesh
    -sENVIRONMENT=web,node
    -sSINGLE_FILE=1
    -sALLOW_MEMORY_GROWTH=1
    -sFILESYSTEM=0)
