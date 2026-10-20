#!/usr/bin/env bash
# Chaos test: kill random nodes in the middle of a load test, bring them back,
# and check that no player was lost or put in two lobbies.
#
#   ./scripts/chaos.sh            against the Docker Compose cluster (builds and starts it)
#   ./scripts/chaos.sh --local    against five node processes run from build/release
#
# Options:
#   --rounds N     how many kill/restart rounds (default 1)
#   --down S       seconds each killed node stays down (default 5)
#   --rate R       players arriving per second during the test (default 500)
#   --players N    total players (default: enough to keep load on for every round)
#   --seed S       picks which nodes die, and the players (default: random)
#
# Each round: kill -9 a random node, time how long the others take to drop it
# from the membership, leave it down, restart it, and time how long it takes
# to be a member again. The load generator runs the whole time, retrying like
# a real client. At the end it checks the invariants (every player in exactly
# one lobby or still queued; nobody lost; nobody in two lobbies). Exit status
# 0 means every invariant held.
#
# Written for the bash 3.2 that ships with macOS (no associative arrays).

set -euo pipefail
cd "$(dirname "$0")/.."

MODE=docker
ROUNDS=1
DOWN=5
RATE=500
PLAYERS=""
SEED=$(( $(date +%s) % 100000 ))
WARMUP=3   # seconds of load before the first kill
SETTLE=5   # seconds between a restart and the next kill (or the end)

while [ $# -gt 0 ]; do
    case "$1" in
        --local) MODE=local; shift ;;
        --rounds) ROUNDS=$2; shift 2 ;;
        --down) DOWN=$2; shift 2 ;;
        --rate) RATE=$2; shift 2 ;;
        --players) PLAYERS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done
RANDOM=$SEED  # makes the choice of victims repeatable for a given seed
if [ -z "$PLAYERS" ]; then
    PLAYERS=$(( RATE * (WARMUP + ROUNDS * (DOWN + SETTLE) + 2) ))
fi

NODES="node-a node-b node-c node-d node-e"
OUT=chaos-results
mkdir -p "$OUT"
rm -f "$OUT"/loadgen.json "$OUT"/loadgen.log

# Milliseconds since the epoch. GNU date can do it; macOS's can't, so use perl.
if [ "$(date +%N)" != "N" ]; then
    now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
else
    now_ms() { perl -MTime::HiRes=time -e 'printf("%d\n", time() * 1000)'; }
fi

seconds() {  # 1520 -> 1.52
    printf '%d.%02d' $(( $1 / 1000 )) $(( ($1 % 1000) / 10 ))
}

# ---- How to start, kill and query nodes, per mode ----

if [ "$MODE" = local ]; then
    BIN=${BUILD_DIR:-build/release}
    for program in node/matchmesh_node node/matchmesh_cli loadgen/matchmesh_loadgen; do
        if [ ! -x "$BIN/$program" ]; then
            echo "missing $BIN/$program; build first: cmake --preset release && cmake --build --preset release" >&2
            exit 1
        fi
    done
    port_of() {
        case $1 in
            node-a) echo 7101 ;; node-b) echo 7102 ;; node-c) echo 7103 ;;
            node-d) echo 7104 ;; node-e) echo 7105 ;;
        esac
    }
    PEERS="" TARGETS=""
    for node in $NODES; do
        PEERS="$PEERS${PEERS:+,}$node=127.0.0.1:$(port_of "$node")"
        TARGETS="$TARGETS${TARGETS:+,}127.0.0.1:$(port_of "$node")"
    done

    start_node() {
        "$BIN/node/matchmesh_node" --id "$1" --listen "127.0.0.1:$(port_of "$1")" \
            --peers "$PEERS" >>"$OUT/$1.log" 2>&1 &
        echo $! >"$OUT/$1.pid"
    }
    kill_node() { kill -9 "$(cat "$OUT/$1.pid")"; }
    ping_node() { "$BIN/node/matchmesh_node" --id chaos --ping "127.0.0.1:$(port_of "$1")" >/dev/null 2>&1; }
    topology() { "$BIN/node/matchmesh_cli" --target "127.0.0.1:$(port_of "$1")" topology 2>/dev/null; }
    start_loadgen() {
        "$BIN/loadgen/matchmesh_loadgen" --targets "$TARGETS" --players "$PLAYERS" \
            --rate "$RATE" --seed "$SEED" --json "$OUT/loadgen.json" >"$OUT/loadgen.log" 2>&1 &
        LOADGEN_PID=$!
    }
    start_cluster() {
        rm -f "$OUT"/node-*.log
        for node in $NODES; do start_node "$node"; done
    }
    stop_cluster() {
        for node in $NODES; do
            [ -f "$OUT/$node.pid" ] && kill "$(cat "$OUT/$node.pid")" 2>/dev/null || true
        done
    }
    trap stop_cluster EXIT
    LOGS_HINT="$OUT/node-*.log"
else
    start_node() { docker compose start "$1" >/dev/null; }
    kill_node() { docker compose kill "$1" >/dev/null; }  # SIGKILL: no clean shutdown
    ping_node() { docker compose exec -T "$1" matchmesh_node --id chaos --ping localhost:50051 >/dev/null 2>&1; }
    topology() { docker compose exec -T "$1" matchmesh_cli --target "$1:50051" topology 2>/dev/null; }
    start_loadgen() {
        docker compose run --rm -v "$PWD/$OUT:/out" loadgen --players "$PLAYERS" \
            --rate "$RATE" --seed "$SEED" --json /out/loadgen.json >"$OUT/loadgen.log" 2>&1 &
        LOADGEN_PID=$!
    }
    start_cluster() { docker compose up -d --build; }
    stop_cluster() { :; }  # leave it running; `docker compose down` stops it
    LOGS_HINT="docker compose logs"
fi

# Waits until "$@" succeeds (checking every 50 ms, for up to $TIMEOUT_MS) and
# prints how many milliseconds it took, or "timeout".
wait_ms() {
    local started elapsed
    started=$(now_ms)
    while ! "$@"; do
        elapsed=$(( $(now_ms) - started ))
        if [ "$elapsed" -ge "${TIMEOUT_MS:-15000}" ]; then
            echo timeout
            return 0
        fi
        sleep 0.05
    done
    echo $(( $(now_ms) - started ))
}

shown_as() {  # shown_as <asker> <node> DOWN|member: is that what <asker> reports?
    topology "$1" | grep -Eq "^[[:space:]]+$2[[:space:]]+$3"
}

# ---- The test ----

echo "==> Starting the cluster ($MODE)"
start_cluster
for node in $NODES; do
    if [ "$(TIMEOUT_MS=60000 wait_ms ping_node "$node")" = timeout ]; then
        echo "    $node never answered; see $LOGS_HINT" >&2
        exit 1
    fi
done
echo "    all five nodes answering"

echo "==> Load: $PLAYERS players at $RATE/s (seed $SEED), $ROUNDS kill round(s), $DOWN s down each"
start_loadgen
sleep "$WARMUP"

SUMMARY=""
for round in $(seq 1 "$ROUNDS"); do
    set -- $NODES
    shift $(( RANDOM % 5 ))
    victim=$1
    asker=node-a
    [ "$victim" = node-a ] && asker=node-b

    killed_at=$(now_ms)
    kill_node "$victim"
    echo "==> Round $round: killed $victim (kill -9)"
    detected=$(wait_ms shown_as "$asker" "$victim" DOWN)
    if [ "$detected" = timeout ]; then
        echo "    !! $asker never dropped $victim from the membership"
        SUMMARY="$SUMMARY\n  round $round: $victim killed, NEVER detected"
    else
        echo "    $asker dropped $victim from the membership after $(seconds "$detected") s"
    fi

    # Leave it down for the rest of $DOWN seconds, then bring it back.
    remaining=$(( DOWN * 1000 - ($(now_ms) - killed_at) ))
    [ "$remaining" -gt 0 ] && sleep "$(seconds "$remaining")"
    start_node "$victim"
    rejoined=$(wait_ms shown_as "$asker" "$victim" member)
    if [ "$rejoined" = timeout ]; then
        echo "    !! $victim never rejoined"
        SUMMARY="$SUMMARY\n  round $round: $victim killed, detected after $(seconds "$detected") s, NEVER rejoined"
    else
        echo "    restarted $victim after $DOWN s down; a member again after $(seconds "$rejoined") s"
        SUMMARY="$SUMMARY\n  round $round: $victim killed, dropped after $(seconds "$detected") s, back in after $(seconds "$rejoined") s"
    fi
    sleep "$SETTLE"
done

echo "==> Waiting for the load test to finish and check the invariants"
if wait "$LOADGEN_PID"; then status=0; else status=$?; fi

sed -n '/^Sent /,$p' "$OUT/loadgen.log"
echo
echo "==> Chaos summary"
printf "%b\n" "${SUMMARY#\\n}"
if [ -f "$OUT/loadgen.json" ]; then
    longest=$(sed -n 's/.*"enqueue_latency": {[^}]*"max_ms": \([0-9.]*\).*/\1/p' "$OUT/loadgen.json")
    retried=$(sed -n 's/.*"players_retried": \([0-9]*\).*/\1/p' "$OUT/loadgen.json")
    echo "  players who had to retry during the outages: $retried; the longest any player waited for \"you're in\": $longest ms"
fi
if [ "$status" -eq 0 ]; then
    echo "PASS: every player is in exactly one lobby or still queued; nobody lost (full log: $OUT/loadgen.log)"
else
    echo "FAIL: see $OUT/loadgen.log and $LOGS_HINT"
fi
exit "$status"
