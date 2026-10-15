# MatchMesh

A fault-tolerant distributed matchmaking cluster: five C++20 nodes, gRPC,
consistent hashing, primary-backup replication and chaos testing.

> Work in progress — currently at Phase 5 (failure detection and replication).

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
- `ClusterService`: `Heartbeat` (nodes exchanging membership), `Replicate`
  (primary to backup, and handing queues to new owners) and `GetTopology` (the
  cluster as this node sees it).

**Any node accepts any request.** The `Router` builds the same hash ring on every
node from the peer list, so each node knows who owns every queue. A request for
another node's queue is forwarded to its owner (once: the forwarded copy carries
an `x-matchmesh-forwarded-by` header and is never forwarded again, so
misconfigured nodes fail fast instead of looping). The response carries
`x-matchmesh-served-by`, naming the node that actually handled it. Forwarded
requests also carry `x-matchmesh-epoch` (see below). If the owner is unreachable
the caller gets `UNAVAILABLE`, which is safe to retry: within about 1.5 s the
owner is dropped from the membership and its backup takes over its queues.

Settings come from flags or environment variables:

| Flag | Variable | Example |
|---|---|---|
| `--id` | `MATCHMESH_NODE_ID` | `node-a` |
| `--listen` | `MATCHMESH_LISTEN` | `0.0.0.0:50051` |
| `--peers` | `MATCHMESH_PEERS` | `node-a=node-a:50051,node-b=node-b:50051,...` |
| `--workers` | `MATCHMESH_WORKERS` | `4` |

The node shuts down cleanly on Ctrl+C or `docker stop`. `matchmesh_cli` is a small
client for trying it by hand (`enqueue`, `get-lobby`, `cancel`, `topology`). Like a
real client it retries `UNAVAILABLE` for up to 10 s and says how many retries it
took. End-to-end tests in [`tests/node/`](tests/node) run three real nodes in-process.

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
- **Incarnations catch fast restarts.** Each process picks a random
  *incarnation* number at startup and sends it in every heartbeat; views record
  each member's incarnation. A node that crashes and is back before anyone
  noticed (under 1.5 s) still has a new incarnation and an empty memory, so the
  coordinator publishes a new epoch for it, which is what gets its queues handed
  back. A process never counts as a member of a view that lists a different
  incarnation of it. (Because epoch 0 lists no incarnations, a freshly started
  cluster settles at epoch 1 once everyone has heard from everyone.)

During a change, two nodes can briefly disagree. A forwarded request carries the
sender's epoch in `x-matchmesh-epoch`; if it differs from the receiver's, the
receiver answers `UNAVAILABLE` ("membership is changing; retry") rather than
the `FAILED_PRECONDITION` it returns when two nodes at the *same* epoch
disagree (which means the peer lists themselves differ).

Peer connections retry quickly (gRPC reconnect backoff 100 ms, capped at 1 s,
instead of the default 1 s growing to 2 min), so a node that was down for a while
is reachable again within about a second of restarting.

## Replication and failover

Each queue lives on two nodes: its **primary** (the owner on the ring) and its
**backup**, the next distinct node clockwise. Replication is **synchronous**: the
primary copies every change to the backup and waits for it to be applied before
the change happens on the primary or the player gets an answer.

- **One path for every change.** An enqueue, a cancel and a lobby forming all go
  *propose → copy to the backup → apply*, under that queue's write lock. If the
  copy fails, nothing changes and the player gets a retryable `UNAVAILABLE`, so a
  player is never told "done" for something only one node knows about. The
  shard's own data lock is not held during the copy, so lookups stay fast.
- **Lobbies are one change.** A lobby is proposed, copied with its unique ID and
  all four players, and only then applied: the players leave the queue and join
  the lobby in a single step on both nodes. A failover can't leave half a lobby
  formed, re-match its players, or put anyone in two lobbies.
- **Strict order.** Every change to a queue has a sequence number. A backup
  applies a change only if it is exactly the next one; anything else (a gap, a
  repeat, a change that doesn't fit) is rejected, and the primary then sends a
  full **snapshot** of the queue before carrying on. The same snapshot brings a
  brand-new backup up to date, and every epoch change re-sends it, so a backup
  that restarted can't be silently empty.
- **Stale nodes can't write.** `Replicate` carries the sender's epoch and is only
  accepted at the receiver's exact epoch, and only from the node the receiver
  believes is that queue's primary. A node that has fallen behind (or was cut
  off) can't overwrite newer state.

**Moving queues when the view changes.** Every node walks the queues it holds:
one it no longer owns is frozen (demoted to a replica), sent to its new owner, and
dropped unless this node is now its backup; one it still owns gets its (possibly
new) backup brought up to date. A node reports through heartbeats when it has
finished, and a queue's **new owner starts serving only once every member has
reported finishing for the current epoch** (until then it answers `UNAVAILABLE`,
retry). That covers both directions:

- *Failover:* the primary dies, the next epoch makes its backup the owner, and
  the backup's copy goes live with every waiting player and every lobby intact.
- *Rejoin:* a node comes back empty, the next epoch gives it its queues back, and
  whoever held them hands them over before it serves.

Measured on a real five-process cluster with the default timings (500 ms
heartbeats, 1.5 s suspicion): `kill -9` of the node owning a queue had that queue
answering again from its backup after **1.3 to 1.5 s** (1,311 ms; 1,499 ms when the
killed node was the coordinator), within the 2 s target. In every run, all 60 players (40 in
lobbies, 20 queued) had exactly the same status and lobby after the crash and
again after the killed node restarted and took its queues back.
[`tests/node/cluster_test.cpp`](tests/node/cluster_test.cpp) checks crashes,
coordinator failover, fast and slow restarts, a backup and then a primary dying,
lobbies surviving failover, stale-epoch rejection and the 2 s target with real
nodes; [`tests/core/replication_test.cpp`](tests/core/replication_test.cpp)
mirrors hundreds of random changes into a second engine and checks both end up
identical.

What this does **not** survive: a queue's primary *and* backup dying within the
same ~1.5 s detection window (only two copies exist). While a dead node hasn't
been noticed yet, changes to the queues it held (as primary or backup) fail
retryably, and new owners wait for it to be dropped before serving.

### Limitation: split brain

This is a simple failure detector, not a consensus protocol. If the network
splits the cluster in two (say `a, b` | `c, d, e`), each side suspects the other,
`node-a` coordinates one side and `node-c` the other, and both keep serving the
queues they now believe they own, each replicating to a backup on its own side.
Epoch checks don't help here, because each side's epochs are internally
consistent. Both sides run happily until the split heals, at which point the
higher epoch wins, its queues are handed over, and whatever the losing side did
to the same queues (players queued, lobbies formed) is overwritten. One-way or
flaky links can also make the view flap. Nodes can't tell "that node crashed"
from "I can't reach that node".

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

To watch failover, put a player in a queue, kill the node holding it, and ask
another node about them:

```bash
docker compose exec node-a matchmesh_cli --target node-a:50051 get-lobby us-4 1204 us-west ranked
#   us-4: PLAYER_STATUS_IN_LOBBY node-d-1 [...]  (asked node-a:50051, served by node-d)
docker compose kill node-d                     # SIGKILL: no clean shutdown
docker compose exec node-a matchmesh_cli --target node-a:50051 get-lobby us-4 1204 us-west ranked
#   same lobby, now served by node-d's backup (after a few retries while it takes over)
docker compose exec node-a matchmesh_cli --target node-a:50051 topology   # node-d DOWN
docker compose start node-d                    # comes back empty and gets its queues back
docker compose logs node-b                     # each epoch and hand-over as it happened
```

(`us-4` is from `cluster_demo.sh`; which node owns its queue depends on the ring,
so kill whichever node the first command says served it.)

## Service contracts

The gRPC APIs live in [`proto/matchmesh/v1/`](proto/matchmesh/v1) (package `matchmesh.v1`).
C++ code is generated from them at build time into `build/<preset>/proto/gen/`
and compiled into the `matchmesh_proto` library.

| File | Contents |
|---|---|
| `common.proto` | Shared types: `Player`, `QueueKey` (the shard key: region, game mode, skill bracket), `Lobby` |
| `matchmaking.proto` | `MatchmakingService`: `Enqueue`, `Cancel`, `GetLobby` (clients → any node) |
| `cluster.proto` | `ClusterService`: `Heartbeat` (membership, incarnations, hand-over progress), `Replicate` (changes and snapshots), `GetTopology` (node ↔ node, gateway) |

## Smoke test

Proves two containers can reach each other over gRPC. Run on your machine, from the repo root,
after building the dev image:

```bash
./scripts/smoke_test.sh
```

It starts `node-a` as a server and has `node-b` send it a `Heartbeat` ping.
