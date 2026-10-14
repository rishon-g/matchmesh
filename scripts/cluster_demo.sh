#!/usr/bin/env bash
# Starts the five-node cluster and shows requests being forwarded.
#
# Run from the repo root on your machine (not inside the dev container):
#   ./scripts/cluster_demo.sh
#
# Every player joins through node-a, yet each one ends up on whichever node
# owns their queue. Then node-e is asked about lobbies it doesn't hold.
set -euo pipefail

cd "$(dirname "$0")/.."

cli() {
    # Run the CLI inside node-a's container, where the other nodes are
    # reachable by name. "$@" is the CLI's arguments.
    docker compose exec -T node-a matchmesh_cli "$@"
}

echo "==> Starting the cluster (the first build takes a few minutes)"
docker compose up -d --build

echo "==> Waiting for all five nodes to answer pings"
for node in node-a node-b node-c node-d node-e; do
    for attempt in $(seq 1 30); do
        if docker compose exec -T node-a matchmesh_node --id demo --ping "$node:50051" >/dev/null 2>&1; then
            echo "    $node is up"
            break
        fi
        if [ "$attempt" -eq 30 ]; then
            echo "    $node never came up; see: docker compose logs $node" >&2
            exit 1
        fi
        sleep 1
    done
done

echo
echo "==> Eight players join, ALL through node-a"
for i in 1 2 3 4; do cli --target node-a:50051 enqueue "us-$i" "$((1200 + i))" us-west ranked; done
for i in 1 2 3 4; do cli --target node-a:50051 enqueue "eu-$i" "$((1700 + i))" eu casual; done

sleep 1  # give the owners' workers a moment to form lobbies

echo
echo "==> Asking node-e, which owns neither queue, where they ended up"
cli --target node-e:50051 get-lobby us-1 1201 us-west ranked
cli --target node-e:50051 get-lobby eu-1 1701 eu casual

echo
echo "Cluster is still running. Try your own:"
echo "  docker compose exec node-a matchmesh_cli --target node-c:50051 enqueue alice 1300 us-west ranked"
echo "Stop it with: docker compose down"
