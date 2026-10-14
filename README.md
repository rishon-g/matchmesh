# MatchMesh

A fault-tolerant distributed matchmaking cluster: five C++20 nodes, gRPC,
consistent hashing, primary-backup replication and chaos testing.

> Work in progress — currently at Phase 5 (failure detection and membership).

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
- `ClusterService`: `Heartbeat` (nodes exchanging membership) and `GetTopology`
  (the cluster as this node sees it).

**Any node accepts any request.** The `Router` builds the same hash ring on every
node from the peer list, so each node knows who owns every queue. A request for
another node's queue is forwarded to its owner (once: the forwarded copy carries
an `x-matchmesh-forwarded-by` header and is never forwarded again, so
misconfigured nodes fail fast instead of looping). The response carries
`x-matchmesh-served-by`, naming the node that actually handled it. Forwarded
requests also carry `x-matchmesh-epoch` (see below). If the owner is unreachable
the caller gets `UNAVAILABLE`, which is safe to retry: within about 1.5 s the
owner is dropped from the membership and its queues move to another node.

Settings come from flags or environment variables:

| Flag | Variable | Example |
|---|---|---|
| `--id` | `MATCHMESH_NODE_ID` | `node-a` |
| `--listen` | `MATCHMESH_LISTEN` | `0.0.0.0:50051` |
| `--peers` | `MATCHMESH_PEERS` | `node-a=node-a:50051,node-b=node-b:50051,...` |
| `--workers` | `MATCHMESH_WORKERS` | `4` |

The node shuts down cleanly on Ctrl+C or `docker stop`. `matchmesh_cli` is a small
client for trying it by hand (`enqueue`, `get-lobby`, `cancel`, `topology`). End-to-end tests in
[`tests/node/`](tests/node) run three real nodes in-process.

## Membership and failure detection

Every node sends a `Heartbeat` to every other node every **500 ms**. Each node
remembers when it last heard from each peer (a heartbeat in either direction);
a peer silent for **1.5 s** is suspected dead. Suspicions are each node's own
opinion, so routing never follows them directly. Instead:

- **The coordinator** is the alive node with the lowest ID (`node-a` while it is
  up). Only it turns its suspicions into an agreed **view**: the sorted list of
  members plus an **epoch** number that goes up by one on every change.
- **Views spread through heartbeats.** Every heartbeat request and reply carries
  the sender's view; a node adopts any view with a higher epoch than its own
  (on a tie, the one from the lower-ID coordinator wins) and passes it on
  immediately. Empty views (as sent by `--ping` health checks) are ignored.
- **Routing follows the view.** Each `Router` rebuilds its hash ring from the
  view's members and swaps it in whole, so a request is routed entirely by one
  epoch or the next, never a mix. Once every node holds the same epoch they all
  route identically, and a dead node's queues have moved to the next node
  clockwise on the ring.
- **Coordinator failure** is handled the same way: when `node-a` goes silent,
  `node-b` becomes the lowest alive node and publishes the next epoch itself.
  A node that comes back is seen again, and the coordinator publishes it back
  in (taking the coordinator role back if it has the lowest ID).

During a change, two nodes can briefly disagree. A forwarded request carries the
sender's epoch in `x-matchmesh-epoch`; if it differs from the receiver's, the
receiver answers `UNAVAILABLE` ("membership is changing; retry") rather than
the `FAILED_PRECONDITION` it returns when two nodes at the *same* epoch
disagree (which means the peer lists themselves differ).

Peer connections retry quickly (gRPC reconnect backoff 100 ms, capped at 1 s,
instead of the default 1 s growing to 2 min), so a node that was down for a while
is reachable again within about a second of restarting.

Measured on a real five-process cluster with the default timings: killing a
node with `kill -9` got it dropped from every node's view in about **1.5 s**
(1,456 ms; 1,480 ms when the killed node was the coordinator), within the 2 s
target. [`tests/node/cluster_test.cpp`](tests/node/cluster_test.cpp) checks
crashes, coordinator failover, restarts and the 2 s target with real nodes.

**Not yet:** players queued on a node that dies are lost, because the new owner
starts with empty queues; primary-backup replication fixes that in the next step.

### Limitation: split brain

This is a simple failure detector, not a consensus protocol. If the network
splits the cluster in two (say `a, b` | `c, d, e`), each side suspects the other,
`node-a` coordinates one side and `node-c` the other, and both keep serving the
queues they now believe they own. Both sides run happily until the split heals,
at which point the higher epoch wins and anything formed on the losing side
for the same queue conflicts. One-way or flaky links can also make the view
flap. Nodes can't tell "that node crashed" from "I can't reach that node".

A consensus protocol such as **Raft** fixes this by requiring a **majority**
(3 of 5) for every membership change: a leader is elected by a majority, and
it may commit a new view only after a majority acknowledges it. The minority
side of a split can never gather a majority, so it stops making changes
instead of forming a second cluster, and every committed view is seen by at
least one node of any future majority. The cost is that a cluster that has
lost its majority stops accepting changes until enough nodes return.

## Five-node cluster

```bash
docker compose up -d --build     # build the slim runtime image and start node-a .. node-e
./scripts/cluster_demo.sh        # every player joins via node-a; watch where they land
docker compose exec node-a matchmesh_cli --target node-c:50051 enqueue alice 1300 us-west ranked
docker compose down
```

To watch failover, stop a node and ask another for its view:

```bash
docker compose stop node-c
docker compose exec node-a matchmesh_cli --target node-a:50051 topology   # epoch 1, node-c DOWN
docker compose start node-c
docker compose exec node-a matchmesh_cli --target node-a:50051 topology   # epoch 2, all members
docker compose logs node-b                                                # each epoch as it arrived
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
