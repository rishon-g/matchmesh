#include "matchmesh/core/skill_queue.hpp"

#include <utility>

namespace matchmesh::core {

SkillQueue::SkillQueue(QueueKey key) : key_(std::move(key)) {}

EnqueueResult SkillQueue::enqueue(Player player, TimePoint now) {
    if (player.id.empty()) {
        return EnqueueResult::kInvalidPlayer;
    }
    if (skill_bracket_for(player.rating) != key_.skill_bracket) {
        return EnqueueResult::kWrongBracket;
    }
    if (by_id_.contains(player.id)) {
        return EnqueueResult::kDuplicate;
    }

    std::string id = player.id;  // copy before `player` is moved from
    // `arrival` is unique, so the insert always succeeds.
    const auto it =
        by_rating_.insert(QueuedPlayer{std::move(player), now, next_arrival_++}).first;
    by_id_.emplace(std::move(id), it);
    return EnqueueResult::kAdded;
}

std::optional<QueuedPlayer> SkillQueue::remove(const std::string& player_id) {
    const auto found = by_id_.find(player_id);
    if (found == by_id_.end()) {
        return std::nullopt;
    }
    // extract() unlinks the entry without copying it, then hands it to us.
    auto node = by_rating_.extract(found->second);
    by_id_.erase(found);
    return std::move(node.value());
}

bool SkillQueue::contains(const std::string& player_id) const {
    return by_id_.contains(player_id);
}

std::size_t SkillQueue::size() const noexcept {
    return by_rating_.size();
}

bool SkillQueue::empty() const noexcept {
    return by_rating_.empty();
}

const QueueKey& SkillQueue::key() const noexcept {
    return key_;
}

SkillQueue::const_iterator SkillQueue::begin() const noexcept {
    return by_rating_.begin();
}

SkillQueue::const_iterator SkillQueue::end() const noexcept {
    return by_rating_.end();
}

}  // namespace matchmesh::core
