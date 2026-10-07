#pragma once

// Matcher: forms lobbies from a SkillQueue.
//
// A lobby forms when lobby_size players' ratings fit within a tolerance
// window (highest rating minus lowest rating <= window). The window starts
// narrow and widens the longer players wait, which trades match quality for
// shorter waits, the same trade real matchmakers make.
//
// The longest-waiting player decides the window: the matcher looks at
// players oldest-first and, for each one, tries the tightest group of
// lobby_size players around them. So a player who has waited a long time
// pulls a wider group together instead of being starved by newcomers who
// still want a narrow match.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "matchmesh/core/skill_queue.hpp"
#include "matchmesh/core/types.hpp"

namespace matchmesh::core {

struct MatchConfig {
    std::size_t lobby_size = 4;          // players per lobby
    std::int32_t initial_window = 50;    // rating spread allowed with no waiting
    std::int32_t widen_per_second = 10;  // extra spread per second waited
    std::int32_t max_window = 300;       // the window never grows past this
};

class Matcher {
public:
    // lobby_id_prefix makes lobby IDs unique across the cluster; the node
    // passes its own ID, e.g. "node-a" -> "node-a-1", "node-a-2", ...
    // Throws std::invalid_argument if the config makes no sense.
    explicit Matcher(std::string lobby_id_prefix, MatchConfig config = {});

    // How wide the window is for a player who has waited this long.
    [[nodiscard]] std::int32_t window_for(Clock::duration waited) const noexcept;

    // Forms as many lobbies as the queue allows right now and removes their
    // players from the queue. `now` is passed in (rather than read from the
    // clock) so tests can control time exactly.
    std::vector<Lobby> match(SkillQueue& queue, TimePoint now);

    [[nodiscard]] const MatchConfig& config() const noexcept;

private:
    std::optional<Lobby> form_one(SkillQueue& queue, TimePoint now);
    std::string next_lobby_id();

    std::string lobby_id_prefix_;
    MatchConfig config_;
    std::uint64_t next_lobby_number_ = 1;
};

}  // namespace matchmesh::core
