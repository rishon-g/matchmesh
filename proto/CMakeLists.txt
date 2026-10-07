# proto: generates C++ code from the .proto contracts and builds it into the
# matchmesh_proto library. Nothing in here is hand-written C++.

# ---- Locate gRPC and Protobuf ----
# We use pkg-config rather than find_package(gRPC CONFIG): Ubuntu 24.04's
# gRPC package ships CMake config files that reference a library that is not
# installed, so find_package fails (grpc/grpc#38838). The .pc files are fine.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)

# The two code generators: protoc (messages) and its gRPC plugin (services).
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)

# ---- Inputs ----
# Paths are relative to this folder, which is the protoc import root, so
# `import "matchmesh/v1/common.proto";` resolves the same way everywhere.
set(MATCHMESH_PROTOS
    matchmesh/v1/common.proto
    matchmesh/v1/matchmaking.proto
    matchmesh/v1/cluster.proto)

# ---- Outputs ----
# Generated files go into the build folder, never the source tree.
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

set(PROTO_ABS_PATHS "")
set(GEN_SRCS "")
set(GEN_HDRS "")
foreach(proto IN LISTS MATCHMESH_PROTOS)
    list(APPEND PROTO_ABS_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/${proto}")
    string(REGEX REPLACE "\\.proto$" "" stem "${proto}")
    list(APPEND GEN_SRCS "${GEN_DIR}/${stem}.pb.cc" "${GEN_DIR}/${stem}.grpc.pb.cc")
    list(APPEND GEN_HDRS "${GEN_DIR}/${stem}.pb.h"  "${GEN_DIR}/${stem}.grpc.pb.h")
endforeach()

# One protoc run regenerates everything. Depending on every .proto means an
# edit to common.proto also refreshes the files that import it.
add_custom_command(
    OUTPUT ${GEN_SRCS} ${GEN_HDRS}
    COMMAND "${PROTOC_EXECUTABLE}"
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${GEN_DIR}
        --grpc_out=${GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
        ${MATCHMESH_PROTOS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS ${PROTO_ABS_PATHS} "${PROTOC_EXECUTABLE}" "${GRPC_CPP_PLUGIN_EXECUTABLE}"
    COMMENT "Generating C++ code from MatchMesh .proto files"
    VERBATIM)

# ---- The library ----
add_library(matchmesh_proto ${GEN_SRCS} ${GEN_HDRS})

# SYSTEM: consumers see generated headers as third-party code, so our strict
# warnings don't fire on code we didn't write. (We also don't link
# matchmesh_warnings here for the same reason.)
target_include_directories(matchmesh_proto SYSTEM PUBLIC "${GEN_DIR}")

# PUBLIC: anything that uses the generated types also needs gRPC/Protobuf.
target_link_libraries(matchmesh_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)
