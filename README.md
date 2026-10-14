# MatchMesh

A fault-tolerant distributed matchmaking cluster: five C++20 nodes, gRPC,
consistent hashing, primary-backup replication and chaos testing.

> Work in progress — currently at Phase 4 (sharding with consistent hashing).

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

# 4. Run a node: serves matchmaking and Heartbeat on port 50051
./build/debug/node/matchmesh_node --id node-a --listen 0.0.0.0:50051 --workers 4
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
- **`MatchmakingEngine`** holds all of a node's queues (one *shard* per queue key)
  and runs a fixed pool of `std::jthread` workers that keep forming lobbies in the
  background. Shards are split between workers by hash, and it is safe to call
  from many threads at once.
- **Lock striping:** every shard has its own mutex, so a busy queue never blocks
  the others. The shard map is behind a reader/writer lock, and a new player
  wakes only the one worker that owns their shard. Compared with a single global
  lock, this roughly doubled end-to-end throughput (about 290k to 580k players/s
  with 4 client threads and 64 queues, on a 2-core machine).
- **`HashRing`** decides which node owns each queue, using consistent hashing.
  Each node is placed on the ring at 100 points (virtual nodes) using a stable
  64-bit hash, and a queue belongs to the first node point clockwise from its own
  hash. The ring depends only on which nodes are in it, so every node computes the
  same answer. `owners(key, 2)` gives the primary and the next distinct node
  clockwise, the backup used for replication.

  Measured with 5 nodes and 10,000 queues: every node owns within about 14% of an
  even share, and removing a node moves only its own queues (about 17%, close to
  the ideal 1/5), where `hash % node_count` would move about 80%.

Unit tests for all of this live in [`tests/core/`](tests/core) (GoogleTest). A
stress suite in [`tests/stress/`](tests/stress) hammers one engine from many
threads at once and checks that nobody is lost or matched twice. Run it under
ThreadSanitizer with `ctest --preset tsan -L stress`.

## Node (`node/`)

Each node is a gRPC server that wraps the engine:

- `MatchmakingService`: `Enqueue`, `Cancel` and `GetLobby` for clients.
- `ClusterService.Heartbeat`: answers pings from other nodes.

**Any node accepts any request.** The `Router` builds the same hash ring on every
node from the peer list, so each node knows who owns every queue. A request for
another node's queue is forwarded to its owner (once: the forwarded copy carries
an `x-matchmesh-forwarded-by` header and is never forwarded again, so
misconfigured nodes fail fast instead of looping). The response carries
`x-matchmesh-served-by`, naming the node that actually handled it. If the owner is
down the caller gets `UNAVAILABLE` within about 2 s; failover arrives in Phase 5.

Settings come from flags or environment variables:

| Flag | Variable | Example |
|---|---|---|
| `--id` | `MATCHMESH_NODE_ID` | `node-a` |
| `--listen` | `MATCHMESH_LISTEN` | `0.0.0.0:50051` |
| `--peers` | `MATCHMESH_PEERS` | `node-a=node-a:50051,node-b=node-b:50051,...` |
| `--workers` | `MATCHMESH_WORKERS` | `4` |

The node shuts down cleanly on Ctrl+C or `docker stop`. `matchmesh_cli` is a small
client for trying it by hand (`enqueue`, `get-lobby`, `cancel`). End-to-end tests in
[`tests/node/`](tests/node) run three real nodes in-process.

## Five-node cluster

```bash
docker compose up -d --build     # build the slim runtime image and start node-a .. node-e
./scripts/cluster_demo.sh        # every player joins via node-a; watch where they land
docker compose exec node-a matchmesh_cli --target node-c:50051 enqueue alice 1300 us-west ranked
docker compose down
```

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
