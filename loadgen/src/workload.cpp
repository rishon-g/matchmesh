#include "matchmesh/loadgen/workload.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace matchmesh::loadgen {
namespace {

// Picks names by weight, e.g. us-west 30% of the time.
class WeightedChoice {
public:
    explicit WeightedChoice(const std::vector<std::pair<std::string, double>>& options) {
        std::vector<double> weights;
        for (const auto& [name, weight] : options) {
            if (name.empty() || !(weight >= 0.0)) {
                throw std::invalid_argument("every option needs a name and a weight >= 0");
            }
            names_.push_back(name);
            weights.push_back(weight);
        }
        if (names_.empty() || std::ranges::none_of(weights, [](double w) { return w > 0.0; })) {
            throw std::invalid_argument("need at least one option with a positive weight");
        }
        pick_ = std::discrete_distribution<std::size_t>(weights.begin(), weights.end());
    }

    const std::string& operator()(std::mt19937_64& rng) { return names_[pick_(rng)]; }

private:
    std::vector<std::string> names_;
    std::discrete_distribution<std::size_t> pick_;
};

}  // namespace

std::vector<PlannedPlayer> make_workload(const WorkloadConfig& config) {
    if (!(config.arrivals_per_second >= 0.0) || !std::isfinite(config.arrivals_per_second)) {
        throw std::invalid_argument("arrivals_per_second must be >= 0");
    }
    if (!(config.rating_stddev >= 0.0) || config.rating_min > config.rating_max) {
        throw std::invalid_argument("rating range or spread makes no sense");
    }

    std::mt19937_64 rng(config.seed);
    WeightedChoice region(config.regions);
    WeightedChoice mode(config.modes);
    std::normal_distribution<double> rating(config.rating_mean, config.rating_stddev);
    // Gaps between Poisson arrivals are exponential, with mean 1 / rate.
    std::exponential_distribution<double> gap_seconds(
        config.arrivals_per_second > 0.0 ? config.arrivals_per_second : 1.0);

    std::vector<PlannedPlayer> players;
    players.reserve(config.players);
    double clock_seconds = 0.0;
    for (std::size_t i = 0; i < config.players; ++i) {
        if (config.arrivals_per_second > 0.0 && i > 0) {
            clock_seconds += gap_seconds(rng);
        }
        PlannedPlayer player;
        player.id = config.id_prefix + std::to_string(i);
        player.rating = std::clamp(static_cast<int>(std::lround(rating(rng))), config.rating_min,
                                   config.rating_max);
        player.region = region(rng);
        player.mode = mode(rng);
        player.arrives_at = std::chrono::microseconds(std::llround(clock_seconds * 1e6));
        players.push_back(std::move(player));
    }
    return players;
}

}  // namespace matchmesh::loadgen
