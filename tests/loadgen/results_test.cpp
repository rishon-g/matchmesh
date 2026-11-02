// Tests for RunResult's summary counts.

#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "matchmesh/loadgen/results.hpp"

namespace matchmesh::loadgen {
namespace {

PlayerResult player(FinalStatus status, std::string lobby_id = "", int attempts = 1) {
    PlayerResult result;
    result.status = status;
    result.lobby_id = std::move(lobby_id);
    result.attempts = attempts;
    return result;
}

TEST(RunResultTest, CountsStatusesDistinctLobbiesAndRetriedPlayers) {
    RunResult run;
    run.players = {player(FinalStatus::kInLobby, "l1"), player(FinalStatus::kInLobby, "l1", 3),
                   player(FinalStatus::kInLobby, "l2"), player(FinalStatus::kQueued, "", 2),
                   player(FinalStatus::kNeverEnqueued, "", 0)};
    EXPECT_EQ(run.count(FinalStatus::kInLobby), 3u);
    EXPECT_EQ(run.count(FinalStatus::kQueued), 1u);
    EXPECT_EQ(run.count(FinalStatus::kNotFound), 0u);
    EXPECT_EQ(run.lobbies(), 2u);
    EXPECT_EQ(run.players_retried(), 2u);
}

}  // namespace
}  // namespace matchmesh::loadgen
