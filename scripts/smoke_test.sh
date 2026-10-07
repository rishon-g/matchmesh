#!/usr/bin/env bash
# Phase 1 smoke test: prove two containers can call each other over gRPC.
#
# Run from the repo root on your machine (not inside the dev container):
#   ./scripts/smoke_test.sh
#
# It builds the node inside the dev image, starts node-a as a server in one
# container, then pings it from node-b in a second container. Both sit on a
# private Docker network, where Docker's DNS lets them find each other by
# container name.
set -euo pipefail

IMAGE="matchmesh-dev"
NETWORK="matchmesh-smoke"
PORT=50051
BINARY="build/debug/node/matchmesh_node"

cd "$(dirname "$0")/.."

cleanup() {
    docker rm -f node-a >/dev/null 2>&1 || true
    docker network rm "$NETWORK" >/dev/null 2>&1 || true
}
trap cleanup EXIT  # always tidy up, even if a step fails
cleanup            # and clear leftovers from an earlier interrupted run

echo "==> Building the node inside $IMAGE"
docker run --rm -v "$PWD":/workspace "$IMAGE" \
    bash -c "cmake --preset debug >/dev/null && cmake --build --preset debug"

echo "==> Starting node-a"
docker network create "$NETWORK" >/dev/null
# --init gives the container a tiny init process so Ctrl-C/stop works promptly.
docker run -d --init --name node-a --network "$NETWORK" \
    -v "$PWD":/workspace "$IMAGE" \
    "$BINARY" --id node-a --listen "0.0.0.0:$PORT" >/dev/null

echo "==> Pinging node-a from node-b"
if docker run --rm --init --name node-b --network "$NETWORK" \
    -v "$PWD":/workspace "$IMAGE" \
    "$BINARY" --id node-b --ping "node-a:$PORT"; then
    echo "==> node-a log:"
    docker logs node-a
    echo "SMOKE TEST PASSED"
else
    echo "==> node-a log:"
    docker logs node-a || true
    echo "SMOKE TEST FAILED" >&2
    exit 1
fi
