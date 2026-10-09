# MatchMesh

A fault-tolerant distributed matchmaking cluster: five C++20 nodes, gRPC,
consistent hashing, primary-backup replication and chaos testing.

> Work in progress — currently at Phase 2 (core matchmaking engine).

## Quick start (development container)

```bash
# 1. Build the dev image (once, or whenever the Dockerfile changes)
docker build -t matchmesh-dev .

# 2. Open a shell inside it, with this folder mounted at /workspace
docker run --rm -it -v "$PWD":/workspace matchmesh-dev

# 3. Inside the container: configure, build, test
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# 4. Run a node (it answers Heartbeat pings on port 50051)
./build/debug/node/matchmesh_node --id node-a --listen 0.0.0.0:50051
```

Presets (configure, build and test): `debug`, `release`, `tsan` (ThreadSanitizer).

## Matchmaking engine (`core/`)

A pure C++ library with no networking, so it can be tested in isolation.

- **Queues** are identified by a `QueueKey`: region, game mode and skill bracket
  (500-point rating bands). The key is also the shard key, so every player who
  could be matched together lives on the same node.
- **`SkillQueue`** keeps one queue's players sorted by rating, so the closest-skill
  neighbours are always adjacent. Duplicate players and players from the wrong
  bracket are rejected.
- **`Matcher`** forms four-player lobbies when the rating spread fits a tolerance
  window. The window starts at 50 points and widens by 10 points per second of
  waiting, up to 300. The longest-waiting player's window decides, so long waits
  aren't starved by newcomers.

Unit tests for all of this live in [`tests/core/`](tests/core) (GoogleTest).

## Service contracts

The gRPC APIs live in [`proto/matchmesh/v1/`](proto/matchmesh/v1) (package `matchmesh.v1`).
C++ code is generated from them at build time into `build/<preset>/proto/gen/`
and compiled into the `matchmesh_proto` library.

| File | Contents |
|---|---|
| `common.proto` | Shared types: `Player`, `QueueKey` (the shard key: region, game mode, skill bracket), `Lobby` |
| `matchmaking.proto` | `MatchmakingService`: `Enqueue`, `Cancel`, `GetLobby` (clients → any node) |
| `cluster.proto` | `ClusterService`: `Heartbeat`, `Replicate`, `GetTopology` (node ↔ node, gateway) |

## Smoke test

Proves two containers can reach each other over gRPC. Run on your machine, from the repo root,
after building the dev image:

```bash
./scripts/smoke_test.sh
```

It starts `node-a` as a server and has `node-b` send it a `Heartbeat` ping.
