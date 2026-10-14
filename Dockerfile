# MatchMesh container images, built in stages from one file.
#
#   docker build -t matchmesh-dev .      → the development container (default)
#   docker compose up --build            → builds the "runtime" stage for each node
#
# Stages:
#   toolchain   Ubuntu 24.04 + compiler, CMake, gRPC, Protobuf, GoogleTest
#   build       compiles the node and CLI in Release mode
#   runtime     a slim image: just the runtime libraries and the two programs
#   dev         the toolchain, with your project mounted at /workspace
#
# `dev` is last, so it is what a plain `docker build` produces, exactly as
# before. Docker only builds the stages the requested target needs.

# ---- toolchain -------------------------------------------------------------
FROM ubuntu:24.04 AS toolchain

# Stop apt from asking interactive questions (e.g. timezone) mid-build.
ENV DEBIAN_FRONTEND=noninteractive

# One RUN step = one image layer. Cleaning the apt cache in the same
# step keeps the image smaller.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        pkg-config \
        ca-certificates \
        libgrpc++-dev \
        libprotobuf-dev \
        protobuf-compiler \
        protobuf-compiler-grpc \
        libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# ---- build -----------------------------------------------------------------
FROM toolchain AS build

WORKDIR /src
# .dockerignore keeps build/ and .git out, so this is just the source.
COPY . .
RUN cmake --preset release -DMATCHMESH_BUILD_TESTS=OFF \
    && cmake --build --preset release --target matchmesh_node matchmesh_cli

# ---- runtime ---------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
# Only the shared libraries the programs load at run time, not the compiler
# or headers. This makes the image a fraction of the toolchain's size.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgrpc++1.51t64 \
        libprotobuf32t64 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/release/node/matchmesh_node /usr/local/bin/
COPY --from=build /src/build/release/node/matchmesh_cli /usr/local/bin/

EXPOSE 50051
# Settings come from MATCHMESH_* environment variables (see docker-compose.yml).
ENTRYPOINT ["matchmesh_node"]

# ---- dev (default) ---------------------------------------------------------
FROM toolchain AS dev

# All later commands run from /workspace. We mount the project here
# at runtime (docker run -v), so edits on your machine show up instantly.
WORKDIR /workspace

CMD ["bash"]
