#pragma once

// HashRing: decides which node owns each queue, using consistent hashing.
//
// Picture a clock face numbered 0 .. 2^64-1. Every node is hashed onto it at
// many points ("virtual nodes", 100 by default), and every queue key is hashed
// onto it too. A key belongs to the first node point found going clockwise
// from the key's position.
//
// Why this way:
//   * Even spread. With 100 points per node, each node owns many small arcs
//     instead of one big one, so load evens out.
//   * Minimal movement. When a node leaves, only the keys on its arcs move
//     (to the next node clockwise); every other key stays where it was. With
//     5 nodes that is about 1/5 of the keys, not nearly all of them as with
//     `hash % node_count`.
//   * Same answer everywhere. The hash is our own fixed function and the ring
//     depends only on *which* nodes are in it, never the order they joined,
//     so every node computes an identical ring.
//
// Not thread-safe: the node layer wraps it in a lock (or swaps whole copies)
// when membership changes.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

// A 64-bit hash that gives the same result on every machine, compiler and run
// (std::hash makes no such promise). FNV-1a over the bytes, followed by the
// MurmurHash3 finalizer so that similar inputs ("node-a#1", "node-a#2") land
// far apart.
[[nodiscard]] std::uint64_t stable_hash(std::string_view data) noexcept;

class HashRing {
public:
    static constexpr std::size_t kDefaultVirtualNodes = 100;

    // Throws std::invalid_argument if virtual_nodes is 0.
    explicit HashRing(std::size_t virtual_nodes = kDefaultVirtualNodes);

    // Adds a node's virtual nodes to the ring. False if it was already there.
    // Throws std::invalid_argument for an empty ID.
    bool add_node(const std::string& node_id);

    // Removes a node and all its virtual nodes. False if it wasn't there.
    bool remove_node(const std::string& node_id);

    [[nodiscard]] bool contains(const std::string& node_id) const;
    [[nodiscard]] std::size_t node_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::vector<std::string> nodes() const;  // sorted by ID
    [[nodiscard]] std::size_t virtual_nodes_per_node() const noexcept;

    // The node that owns `key`, or nothing if the ring is empty.
    [[nodiscard]] std::optional<std::string> owner(std::string_view key) const;
    [[nodiscard]] std::optional<std::string> owner(const QueueKey& key) const;

    // Up to `count` *distinct* nodes for `key`, in clockwise order: the owner
    // first, then the next different node, and so on. owners(key, 2)[1] is
    // the key's backup node (used for replication in Phase 5). Returns fewer
    // than `count` if the ring has fewer nodes.
    [[nodiscard]] std::vector<std::string> owners(std::string_view key, std::size_t count) const;
    [[nodiscard]] std::vector<std::string> owners(const QueueKey& key, std::size_t count) const;

private:
    // Puts one node's virtual nodes on the ring.
    void place(const std::string& node_id);

    std::size_t virtual_nodes_;
    std::set<std::string> nodes_;
    // Position on the ring -> node at that position. A std::map is sorted, so
    // "first point clockwise from h" is just lower_bound(h).
    std::map<std::uint64_t, std::string> ring_;
};

}  // namespace matchmesh::core
