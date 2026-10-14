#include "matchmesh/core/hash_ring.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace matchmesh::core {

std::uint64_t stable_hash(std::string_view data) noexcept {
    // FNV-1a, 64-bit: start from a fixed "offset basis", and for each byte
    // XOR it in, then multiply by a fixed prime.
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    // MurmurHash3's 64-bit finalizer ("fmix64"): mixes every input bit into
    // every output bit, so keys that differ by one character end up at
    // unrelated positions on the ring.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

HashRing::HashRing(std::size_t virtual_nodes) : virtual_nodes_(virtual_nodes) {
    if (virtual_nodes_ == 0) {
        throw std::invalid_argument("a node needs at least one virtual node");
    }
}

bool HashRing::add_node(const std::string& node_id) {
    if (node_id.empty()) {
        throw std::invalid_argument("node ID must not be empty");
    }
    if (!nodes_.insert(node_id).second) {
        return false;  // already on the ring
    }
    place(node_id);
    return true;
}

bool HashRing::remove_node(const std::string& node_id) {
    if (nodes_.erase(node_id) == 0) {
        return false;
    }
    // Rebuild rather than erase this node's points one by one. In the
    // astronomically rare case of two virtual nodes hashing to the same spot,
    // the loser's point was never stored; rebuilding brings it back, so the
    // ring always matches what it would be had this node never joined.
    ring_.clear();
    for (const std::string& node : nodes_) {
        place(node);
    }
    return true;
}

void HashRing::place(const std::string& node_id) {
    for (std::size_t i = 0; i < virtual_nodes_; ++i) {
        // "node-a#0", "node-a#1", ... each lands somewhere different.
        const std::uint64_t point = stable_hash(node_id + "#" + std::to_string(i));
        auto [it, inserted] = ring_.try_emplace(point, node_id);
        // Collision: the smaller ID always wins, whichever joined first, so
        // every node still computes exactly the same ring.
        if (!inserted && node_id < it->second) {
            it->second = node_id;
        }
    }
}

bool HashRing::contains(const std::string& node_id) const {
    return nodes_.contains(node_id);
}

std::size_t HashRing::node_count() const noexcept {
    return nodes_.size();
}

bool HashRing::empty() const noexcept {
    return nodes_.empty();
}

std::vector<std::string> HashRing::nodes() const {
    return {nodes_.begin(), nodes_.end()};
}

std::size_t HashRing::virtual_nodes_per_node() const noexcept {
    return virtual_nodes_;
}

std::optional<std::string> HashRing::owner(std::string_view key) const {
    if (ring_.empty()) {
        return std::nullopt;
    }
    // First point at or after the key's position; past the last point, wrap
    // around to the first. That wrap is what makes it a ring.
    auto it = ring_.lower_bound(stable_hash(key));
    if (it == ring_.end()) {
        it = ring_.begin();
    }
    return it->second;
}

std::optional<std::string> HashRing::owner(const QueueKey& key) const {
    return owner(to_string(key));
}

std::vector<std::string> HashRing::owners(std::string_view key, std::size_t count) const {
    std::vector<std::string> result;
    if (ring_.empty() || count == 0) {
        return result;
    }
    const std::size_t wanted = std::min(count, nodes_.size());
    result.reserve(wanted);

    // Walk clockwise, skipping points that belong to a node we already have.
    // At most one full lap of the ring.
    auto it = ring_.lower_bound(stable_hash(key));
    for (std::size_t steps = 0; steps < ring_.size() && result.size() < wanted; ++steps) {
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        bool seen = false;
        for (const std::string& node : result) {
            seen = seen || node == it->second;
        }
        if (!seen) {
            result.push_back(it->second);
        }
        ++it;
    }
    return result;
}

std::vector<std::string> HashRing::owners(const QueueKey& key, std::size_t count) const {
    return owners(to_string(key), count);
}

}  // namespace matchmesh::core
