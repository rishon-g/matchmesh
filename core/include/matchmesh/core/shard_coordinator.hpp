#pragma once

// The decisions that keep each queue's data on the nodes the ring says
// should hold it, with the network taken out. A real node wraps these in
// gRPC (node::ReplicationClient, node::ShardManager); the in-browser
// simulator wraps them in an in-memory network. Both run this exact code.
//
//   ReplicaTransport  "send this change / this copy to that node, under this
//                     epoch; did it accept?" Implemented per transport.
//
//   BackupReplicator  the engine's core::Replicator: ships each change to the
//                     queue's backup, sending a full copy first whenever the
//                     backup may not hold one.
//
//   ShardCoordinator  everything around replication:
//     * receiving: accepts a change only from the queue's primary, in order,
//       under the exact current epoch; a copy replaces a backup's replica or
//       hands a queue to its new owner (newest copy wins).
//     * handing over: after a view change, every queue this node no longer
//       owns is demoted, sent to its new owner, and dropped (unless this node
//       is now its backup).
//     * taking over: a queue that became this node's is served only once
//       every member reports it has finished handing over for this epoch.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/core/placement.hpp"
#include "matchmesh/core/replication.hpp"
#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

// Carries replication messages from this node to another. Each message is
// tagged with the sender (this node) and the epoch it was sent under. True
// only if the receiver accepted it; a lost reply counts as a refusal.
class ReplicaTransport {
public:
    virtual ~ReplicaTransport() = default;

    virtual bool send_change(const std::string& target, std::uint64_t epoch,
                             const ShardChange& change) = 0;
    virtual bool send_snapshot(const std::string& target, std::uint64_t epoch,
                               const ShardState& state) = 0;
};

class BackupReplicator final : public Replicator {
public:
    // Both must outlive this.
    BackupReplicator(const PlacementSource& placement, ReplicaTransport& transport);

    // Both refuse (return false) if, under the current view, this node isn't
    // the queue's primary any more.
    bool replicate(const ShardChange& change, const std::function<ShardState()>& current) override;
    bool sync(const QueueKey& queue, const std::function<ShardState()>& current) override;

private:
    // Whether `backup` is known to hold a complete copy of `queue` as of
    // `epoch`, and recording that it does (or forgetting it).
    bool is_synced(const QueueKey& queue, const std::string& backup, std::uint64_t epoch);
    void mark_synced(const QueueKey& queue, const std::string& backup, std::uint64_t epoch);
    void forget(const QueueKey& queue);

    struct SyncedCopy {
        std::string backup;
        std::uint64_t epoch = 0;
    };

    const PlacementSource& placement_;
    ReplicaTransport& transport_;

    std::mutex mutex_;  // guards synced_
    std::unordered_map<QueueKey, SyncedCopy, QueueKeyHash> synced_;
};

// The answer to a replication message.
struct ReplicaVerdict {
    bool accepted = false;
    std::string reason;      // why not, when refused
    std::uint64_t epoch = 0; // the receiver's epoch, so a stale sender can tell
};

// What one pass of handing over and taking over did.
struct CoordinatorPass {
    std::uint64_t epoch = 0;                    // the view the pass ran under
    bool finished_hand_over = false;            // synced_epoch() just became `epoch`
    std::size_t copies_sent = 0;                // queue copies handed to new owners
    std::size_t promoted = 0;                   // copies that started serving here
};

// A queue this node is serving, for topology reports.
struct ServedShard {
    QueueKey queue;
    std::optional<std::string> backup;
    std::size_t queued = 0;
};

class ShardCoordinator {
public:
    // `incarnation` identifies this process (see core::Membership). All three
    // references must outlive this.
    ShardCoordinator(MatchmakingEngine& engine, const PlacementSource& placement,
                     ReplicaTransport& transport, std::uint64_t incarnation);

    // A peer reported the latest epoch it has finished handing over for
    // (nothing if it hasn't finished any yet, e.g. it just restarted).
    void record_progress(const std::string& node, std::optional<std::uint64_t> synced_epoch);
    // Ours, to report to peers.
    [[nodiscard]] std::optional<std::uint64_t> synced_epoch() const;

    // True when `view` lists this exact process and every member has
    // reported finishing its hand-over for view.epoch.
    [[nodiscard]] bool settled(const MembershipView& view) const;

    // The placement says this node owns `queue`. Nothing if a client request
    // for it may be served here right now; otherwise why not (the client
    // should retry). Promotes a waiting copy once the hand-over is finished.
    [[nodiscard]] std::optional<std::string> admit(const QueueKey& queue);

    // A replication message from `sender`, sent under `epoch`.
    ReplicaVerdict receive(const std::string& sender, std::uint64_t epoch,
                           const ShardChange& change);
    ReplicaVerdict receive(const std::string& sender, std::uint64_t epoch,
                           const ShardState& state);

    // One pass of the background work: hand over what this node no longer
    // owns (if not done for the current epoch yet), promote what it now owns
    // once settled, and bring every backup up to date. Anything that failed
    // is retried by the next pass.
    CoordinatorPass run_pass();

    // The queues this node is serving as primary right now.
    [[nodiscard]] std::vector<ServedShard> served_shards() const;

private:
    // Hands every queue this node no longer owns to its owner, counting the
    // copies sent in `sent`. True if all were accepted and the view is still
    // `view`.
    bool hand_over_all(const MembershipView& view, std::size_t& sent);
    std::size_t promote_ready(const MembershipView& view);
    void sync_backups();

    MatchmakingEngine& engine_;
    const PlacementSource& placement_;
    ReplicaTransport& transport_;
    const std::uint64_t incarnation_;

    mutable std::mutex mutex_;  // guards progress_ and synced_epoch_
    std::map<std::string, std::uint64_t> progress_;  // peer -> its synced epoch
    std::optional<std::uint64_t> synced_epoch_;
};

}  // namespace matchmesh::core
