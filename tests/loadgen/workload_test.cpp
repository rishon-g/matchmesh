// Tests for the workload generator: the right number of players, arriving
// at the right average rate, with realistic ratings, the same every time
// for a given seed.

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "matchmesh/loadgen/workload.hpp"

namespace matchmesh::loadgen {
namespace {

using namespace std::chrono_literals;

TEST(WorkloadTest, MakesTheRequestedPlayersWithUniqueIdsInArrivalOrder) {
    const auto players = make_workload({.players = 10'000});
    ASSERT_EQ(players.size(), 10'000u);
    std::set<std::string> ids;
    for (const auto& player : players) {
        ids.insert(player.id);
    }
    EXPECT_EQ(ids.size(), players.size());
    EXPECT_TRUE(std::ranges::is_sorted(players, {}, &PlannedPlayer::arrives_at));
    EXPECT_EQ(players.front().arrives_at, 0us);
}

TEST(WorkloadTest, ArrivalsAverageTheTargetRate) {
    const auto players = make_workload({.players = 10'000, .arrivals_per_second = 1'000});
    // 10,000 arrivals at 1,000/s: about 10 s, within a few percent.
    const double seconds = std::chrono::duration<double>(players.back().arrives_at).count();
    EXPECT_NEAR(seconds, 10.0, 0.4);
}

TEST(WorkloadTest, ArrivalsAreBurstyNotEvenlySpaced) {
    // A Poisson process: gaps vary, with standard deviation equal to the mean.
    const auto players = make_workload({.players = 10'000, .arrivals_per_second = 1'000});
    std::vector<double> gaps;
    for (std::size_t i = 1; i < players.size(); ++i) {
        gaps.push_back(static_cast<double>((players[i].arrives_at - players[i - 1].arrives_at).count()));
    }
    const double mean = std::accumulate(gaps.begin(), gaps.end(), 0.0) / static_cast<double>(gaps.size());
    double variance = 0;
    for (const double gap : gaps) {
        variance += (gap - mean) * (gap - mean);
    }
    const double stddev = std::sqrt(variance / static_cast<double>(gaps.size()));
    EXPECT_NEAR(mean, 1'000.0, 50.0);  // 1 ms in microseconds
    EXPECT_NEAR(stddev / mean, 1.0, 0.1);
}

TEST(WorkloadTest, RateZeroSendsEveryoneAtOnce) {
    const auto players = make_workload({.players = 100, .arrivals_per_second = 0});
    EXPECT_TRUE(std::ranges::all_of(players, [](const auto& p) { return p.arrives_at == 0us; }));
}

TEST(WorkloadTest, RatingsFollowABellCurveWithinTheLimits) {
    const auto players = make_workload({.players = 20'000});
    double sum = 0;
    double sum_sq = 0;
    for (const auto& player : players) {
        ASSERT_GE(player.rating, 0);
        ASSERT_LE(player.rating, 3'000);
        sum += player.rating;
        sum_sq += static_cast<double>(player.rating) * player.rating;
    }
    const double n = static_cast<double>(players.size());
    const double mean = sum / n;
    EXPECT_NEAR(mean, 1'500.0, 10.0);
    EXPECT_NEAR(std::sqrt(sum_sq / n - mean * mean), 300.0, 10.0);
}

TEST(WorkloadTest, RegionsAndModesFollowTheirWeights) {
    const auto players = make_workload({.players = 20'000});
    std::map<std::string, double> regions;
    std::map<std::string, double> modes;
    for (const auto& player : players) {
        ++regions[player.region];
        ++modes[player.mode];
    }
    const double n = static_cast<double>(players.size());
    EXPECT_NEAR(regions["us-west"] / n, 0.30, 0.02);
    EXPECT_NEAR(regions["asia"] / n, 0.15, 0.02);
    EXPECT_NEAR(modes["ranked"] / n, 0.60, 0.02);
    EXPECT_EQ(regions.size(), 4u);
}

TEST(WorkloadTest, TheSameSeedGivesTheSameWorkload) {
    const auto a = make_workload({.players = 500, .seed = 7});
    const auto b = make_workload({.players = 500, .seed = 7});
    const auto c = make_workload({.players = 500, .seed = 8});
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i].rating, b[i].rating);
        ASSERT_EQ(a[i].region, b[i].region);
        ASSERT_EQ(a[i].arrives_at, b[i].arrives_at);
    }
    const bool any_different = !std::ranges::equal(a, c, {}, &PlannedPlayer::rating,
                                                   &PlannedPlayer::rating);
    EXPECT_TRUE(any_different);
}

TEST(WorkloadTest, NonsenseConfigsAreRejected) {
    EXPECT_THROW((void)make_workload({.arrivals_per_second = -1}), std::invalid_argument);
    EXPECT_THROW((void)make_workload({.rating_min = 10, .rating_max = 5}), std::invalid_argument);
    WorkloadConfig no_regions;
    no_regions.regions = {{"eu", 0.0}};
    EXPECT_THROW((void)make_workload(no_regions), std::invalid_argument);
}

}  // namespace
}  // namespace matchmesh::loadgen
