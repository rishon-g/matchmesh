# node: the server process. Later this becomes the gRPC server.
add_executable(matchmesh_node src/main.cpp)
target_link_libraries(matchmesh_node PRIVATE matchmesh_core matchmesh_proto matchmesh_warnings)
