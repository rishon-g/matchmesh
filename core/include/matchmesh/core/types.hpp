#pragma once

// Plain data types shared by the matchmaking engine.
// Deliberately free of any networking or protobuf code: core only knows
// about players, queues and lobbies.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matchmesh::core {

// steady_clock only ever moves forward, so wait times can't go negative if
// the machine's wall clock is adjusted.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// Width of one skill bracket in rating points: 0-499 is bracket 0,
// 500-999 is bracket 1, and so on.
inline constexpr std::int32_t kSkillBracketWidth = 500;

struct Player {
    std::string id;
    std::int32_t rating = 0;

    bool operator==(const Player&) const = default;
};

// Identifies one queue. It is also the shard key: in Phase 4 the hash ring
// decides which node owns a queue by hashing this key. Players are sharded
// by queue, not by player, because players who can be matched together must
// all live on the same node.
struct QueueKey {
    std::string region;     // e.g. "us-west"
    std::string game_mode;  // e.g. "ranked"
    std::int32_t skill_bracket = 0;

    bool operator==(const QueueKey&) const = default;
};

// The bracket a rating falls into. Negative ratings are treated as bracket 0.
[[nodiscard]] std::int32_t skill_bracket_for(std::int32_t rating) noexcept;

// The queue a player with this rating joins for a region and game mode.
[[nodiscard]] QueueKey make_queue_key(std::string region, std::string game_mode,
                                      std::int32_t rating);

// Human-readable form such as "us-west/ranked/2". Also a stable string to
// hash onto the ring later.
[[nodiscard]] std::string to_string(const QueueKey& key);

// Lets QueueKey be used as the key of a std::unordered_map.
struct QueueKeyHash {
    [[nodiscard]] std::size_t operator()(const QueueKey& key) const noexcept;
};

// A group of players the matcher placed together.
struct Lobby {
    std::string lobby_id;         // unique, so replication can apply it exactly once
    QueueKey queue;
    std::vector<Player> players;  // ordered by rating, lowest first
    TimePoint formed_at;
};

}  // namespace matchmesh::core
