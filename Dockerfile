# MatchMesh development container.
# Every build runs inside this image, so the compiler and library
# versions are identical on every machine.

FROM ubuntu:24.04

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

# All later commands run from /workspace. We mount the project here
# at runtime (docker run -v), so edits on your machine show up instantly.
WORKDIR /workspace

CMD ["bash"]
