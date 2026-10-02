# MatchMesh

A fault-tolerant distributed matchmaking cluster: five C++20 nodes, gRPC,
consistent hashing, primary-backup replication and chaos testing.

> Work in progress — currently at Phase 1 (toolchain and repository setup).

## Quick start (development container)

```bash
# 1. Build the dev image (once, or whenever the Dockerfile changes)
docker build -t matchmesh-dev .

# 2. Open a shell inside it, with this folder mounted at /workspace
docker run --rm -it -v "$PWD":/workspace matchmesh-dev

# 3. Inside the container: configure, build, run
cmake --preset debug
cmake --build --preset debug
./build/debug/node/matchmesh_node
```

Build presets: `debug`, `release`, `tsan` (ThreadSanitizer).
