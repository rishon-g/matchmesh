#include "matchmesh/core/types.hpp"

#include <functional>
#include <utility>

namespace matchmesh::core {

std::int32_t skill_bracket_for(std::int32_t rating) noexcept {
    return rating < 0 ? 0 : rating / kSkillBracketWidth;
}

QueueKey make_queue_key(std::string region, std::string game_mode, std::int32_t rating) {
    return QueueKey{std::move(region), std::move(game_mode), skill_bracket_for(rating)};
}

std::string to_string(const QueueKey& key) {
    return key.region + "/" + key.game_mode + "/" + std::to_string(key.skill_bracket);
}

std::size_t QueueKeyHash::operator()(const QueueKey& key) const noexcept {
    // Combine the three field hashes (the same mixing step Boost's
    // hash_combine uses) so keys that differ in any field spread out.
    std::size_t seed = std::hash<std::string>{}(key.region);
    const auto combine = [&seed](std::size_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };
    combine(std::hash<std::string>{}(key.game_mode));
    combine(std::hash<std::int32_t>{}(key.skill_bracket));
    return seed;
}

}  // namespace matchmesh::core
