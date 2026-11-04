#include "matchmesh/core/placement.hpp"

#include <utility>
#include <vector>

namespace matchmesh::core {

Placement place(const HashRing& ring, std::uint64_t epoch, const QueueKey& queue) {
    std::vector<std::string> owners = ring.owners(queue, 2);
    Placement placement{std::move(owners.front()), std::nullopt, epoch};
    if (owners.size() > 1) {
        placement.backup = std::move(owners[1]);
    }
    return placement;
}

}  // namespace matchmesh::core
