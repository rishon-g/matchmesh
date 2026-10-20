#pragma once

// Primary-backup replication: the changes a shard goes through, a complete
// copy of a shard, and the interface the engine uses to ship changes to a
// backup. The engine produces and applies these; the node layer moves them
// over the network. Still no networking in core.
//
// Every change to a shard follows the same three steps, all under that
// shard's write lock:
//
//   1. propose  work out the change (a player joins or leaves, or a lobby
//               forms) without touching the shard
//   2. copy     hand it to the Replicator, which sends it to the backup and
//               waits until the backup has applied it
//   3. apply    make the change locally, exactly as the backup did
//
// If step 2 fails nothing changes, so a change is only ever visible on the
// primary once the backup has it too. That is what lets the backup take over
// without losing anything a player was told succeeded.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

// A player joined the queue at `enqueued_at` (sent along so the backup's
// copy keeps the same wait time, which decides the player's match window).
struct PlayerJoined {
    Player player;
    TimePoint enqueued_at;
    std::string request_id{};  // the client's idempotency key, if it sent one
};

// Which request put a player where they are now (queued, or in a lobby), so a
// retry of that same request can be recognised and answered "already done".
struct JoinTicket {
    std::string request_id;
    TimePoint enqueued_at;

    bool operator==(const JoinTicket&) const = default;
};

// A player left the queue (cancelled).
struct PlayerLeft {
    std::string player_id;
};

// A lobby formed: its players leave the queue and the lobby is recorded, as
// one step. Applied whole or not at all, so a failover can never leave some
// of its players queued and others in the lobby.
struct LobbyFormed {
    Lobby lobby;
};

// One change to one shard. Sequence numbers start at 1 and go up by one per
// change; a backup applies a change only if it is the very next one, so
// changes can't be lost, reordered or applied twice.
struct ShardChange {
    QueueKey queue;
    std::uint64_t sequence = 0;
    std::variant<PlayerJoined, PlayerLeft, LobbyFormed> op;
};

// A complete copy of one shard, as of change number `sequence`. Sent when a
// backup is new or may have missed changes, and when a shard moves to a new
// owner.
struct ShardState {
    QueueKey queue;
    std::uint64_t sequence = 0;
    std::vector<PlayerJoined> waiting;  // in arrival order
    std::vector<Lobby> lobbies;         // lobbies someone still belongs to
    // Player ID -> lobby ID, for "which lobby is this player in?". Not every
    // member of a lobby is listed: one who has since queued again isn't.
    std::map<std::string, std::string> lobby_of;
    // Player ID -> the join that put them in the queue or their lobby (only
    // for joins that carried a request ID).
    std::map<std::string, JoinTicket> tickets{};
};

// Ships a shard's changes to its backup. Implemented by the node layer.
//
// The engine calls these with the shard's write lock held, so for any one
// shard the calls never overlap and changes arrive in sequence order.
// `current` returns the shard's full state before the change, in case the
// backup needs a complete copy first.
class Replicator {
public:
    virtual ~Replicator() = default;

    // Copies `change` to the backup and waits until it has been applied
    // there. Returns false if it wasn't (the backup is unreachable or
    // refused, or this node no longer owns the shard); the engine then
    // leaves the shard unchanged.
    virtual bool replicate(const ShardChange& change,
                           const std::function<ShardState()>& current) = 0;

    // Makes sure the backup holds a complete, up-to-date copy, sending one
    // if it might not. Returns false if that couldn't be confirmed.
    virtual bool sync(const QueueKey& queue, const std::function<ShardState()>& current) = 0;
};

}  // namespace matchmesh::core
