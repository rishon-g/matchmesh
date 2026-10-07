#pragma once

// SkillQueue: the players waiting in one queue, kept sorted by rating so the
// closest-skill neighbours are always next to each other.
//
// Not thread-safe on its own. Phase 3 puts one mutex in front of each queue
// (lock striping).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

// A player plus when they joined this queue.
struct QueuedPlayer {
    Player player;
    TimePoint enqueued_at;
    // Increases by one per enqueue in this queue. Breaks ties between equal
    // ratings so that, among equals, whoever arrived first comes first.
    std::uint64_t arrival = 0;
};

enum class EnqueueResult {
    kAdded,          // the player is now waiting
    kDuplicate,      // a player with this ID is already in the queue
    kWrongBracket,   // the rating belongs to a different skill bracket
    kInvalidPlayer,  // the player ID is empty
};

class SkillQueue {
    // Sort order: rating, then arrival.
    struct ByRating {
        bool operator()(const QueuedPlayer& a, const QueuedPlayer& b) const noexcept {
            if (a.player.rating != b.player.rating) {
                return a.player.rating < b.player.rating;
            }
            return a.arrival < b.arrival;
        }
    };
    using Ordered = std::set<QueuedPlayer, ByRating>;

public:
    // Iterating a SkillQueue visits players from lowest to highest rating.
    using const_iterator = Ordered::const_iterator;

    explicit SkillQueue(QueueKey key);

    // Adds a player who joined at time `now`.
    [[nodiscard]] EnqueueResult enqueue(Player player, TimePoint now);

    // Takes a player out of the queue (a cancel, or because they were placed
    // in a lobby). Returns the removed entry, or nothing if they weren't here.
    std::optional<QueuedPlayer> remove(const std::string& player_id);

    [[nodiscard]] bool contains(const std::string& player_id) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const QueueKey& key() const noexcept;

    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;

private:
    QueueKey key_;
    Ordered by_rating_;  // the players, sorted
    // Player ID -> position in by_rating_, so lookups and removals by ID
    // don't have to scan the whole queue. std::set iterators stay valid when
    // other elements are inserted or erased, so these never go stale.
    std::unordered_map<std::string, const_iterator> by_id_;
    std::uint64_t next_arrival_ = 0;
};

}  // namespace matchmesh::core
