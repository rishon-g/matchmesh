#pragma once

// Where a queue lives, and the read-only view of the cluster that the
// replication logic needs to decide it. The node layer answers these from its
// router; the in-browser simulator answers them from its own ring. Either way
// the decisions made from them (see shard_coordinator.hpp) are the same code.

#include <cstdint>
#include <optional>
#include <string>

#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

// Where a queue lives under one view: its primary (the owner) and its backup,
// the next distinct node clockwise on the ring (none in a one-node cluster).
struct Placement {
    std::string primary;
    std::optional<std::string> backup;
    std::uint64_t epoch = 0;
};

// The placement of `queue` on `ring`, which was built from the view numbered
// `epoch`. The ring must have at least one node.
[[nodiscard]] Placement place(const HashRing& ring, std::uint64_t epoch, const QueueKey& queue);

// One node's current idea of the cluster: who it is, the view it routes by,
// and where each queue lives under that view. Implementations must answer
// placement() and view() from the same view at any one moment, or report the
// epoch alongside (Placement does), so callers can tell the two apart.
class PlacementSource {
public:
    virtual ~PlacementSource() = default;

    [[nodiscard]] virtual const std::string& self_id() const = 0;
    [[nodiscard]] virtual Placement placement(const QueueKey& queue) const = 0;
    [[nodiscard]] virtual MembershipView view() const = 0;
};

}  // namespace matchmesh::core
