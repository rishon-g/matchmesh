#include "matchmesh/core/matcher.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace matchmesh::core {

Matcher::Matcher(std::string lobby_id_prefix, MatchConfig config)
    : lobby_id_prefix_(std::move(lobby_id_prefix)), config_(config) {
    if (config_.lobby_size < 2) {
        throw std::invalid_argument("lobby_size must be at least 2");
    }
    if (config_.initial_window < 0 || config_.widen_per_second < 0) {
        throw std::invalid_argument("windows and widening rate must not be negative");
    }
    if (config_.max_window < config_.initial_window) {
        throw std::invalid_argument("max_window must be at least initial_window");
    }
}

std::int32_t Matcher::window_for(Clock::duration waited) const noexcept {
    std::int64_t waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(waited).count();
    if (waited_ms <= 0) {
        return config_.initial_window;
    }
    // Cap the wait before multiplying so the arithmetic can't overflow.
    // 10^9 ms is about 11.5 days, far past the point the window maxes out.
    waited_ms = std::min<std::int64_t>(waited_ms, 1'000'000'000);

    const std::int64_t widened =
        config_.initial_window + config_.widen_per_second * waited_ms / 1000;
    return static_cast<std::int32_t>(std::min<std::int64_t>(widened, config_.max_window));
}

std::vector<Lobby> Matcher::match(SkillQueue& queue, TimePoint now) {
    std::vector<Lobby> lobbies;
    while (auto lobby = form_one(queue, now)) {
        lobbies.push_back(std::move(*lobby));
    }
    return lobbies;
}

const MatchConfig& Matcher::config() const noexcept {
    return config_;
}

std::optional<Lobby> Matcher::form_one(SkillQueue& queue, TimePoint now) {
    const std::size_t k = config_.lobby_size;
    const std::size_t n = queue.size();
    if (n < k) {
        return std::nullopt;
    }

    // The queue iterates in rating order, so sorted[i] is the i-th lowest rating.
    std::vector<const QueuedPlayer*> sorted;
    sorted.reserve(n);
    for (const QueuedPlayer& entry : queue) {
        sorted.push_back(&entry);
    }

    // Visit players longest-waiting first.
    std::vector<std::size_t> by_wait(n);
    std::iota(by_wait.begin(), by_wait.end(), std::size_t{0});
    std::ranges::sort(by_wait, [&sorted](std::size_t a, std::size_t b) {
        if (sorted[a]->enqueued_at != sorted[b]->enqueued_at) {
            return sorted[a]->enqueued_at < sorted[b]->enqueued_at;
        }
        return sorted[a]->arrival < sorted[b]->arrival;
    });

    for (const std::size_t anchor : by_wait) {
        const std::int64_t window = window_for(now - sorted[anchor]->enqueued_at);

        // Because the list is sorted, the tightest group of k players that
        // includes the anchor is always k neighbours in a row. Try every run
        // of k that contains the anchor and keep the one with the smallest
        // spread.
        const std::size_t first_start = anchor >= k - 1 ? anchor - (k - 1) : 0;
        const std::size_t last_start = std::min(anchor, n - k);

        std::size_t best_start = first_start;
        std::int64_t best_spread = std::numeric_limits<std::int64_t>::max();
        for (std::size_t start = first_start; start <= last_start; ++start) {
            const std::int64_t spread =
                static_cast<std::int64_t>(sorted[start + k - 1]->player.rating) -
                sorted[start]->player.rating;
            if (spread < best_spread) {
                best_spread = spread;
                best_start = start;
            }
        }
        if (best_spread > window) {
            continue;  // no group fits this player's window yet
        }

        Lobby lobby;
        lobby.lobby_id = next_lobby_id();
        lobby.queue = queue.key();
        lobby.formed_at = now;
        lobby.players.reserve(k);
        for (std::size_t i = best_start; i < best_start + k; ++i) {
            lobby.players.push_back(sorted[i]->player);
        }
        // Copy the players out first: removing them from the queue destroys
        // the entries that `sorted` points to.
        for (const Player& player : lobby.players) {
            queue.remove(player.id);
        }
        return lobby;
    }
    return std::nullopt;
}

std::string Matcher::next_lobby_id() {
    return lobby_id_prefix_ + "-" + std::to_string(next_lobby_number_++);
}

}  // namespace matchmesh::core
