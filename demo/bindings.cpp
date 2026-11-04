// JavaScript bindings for the simulated cluster (compiled only with
// Emscripten). The dashboard's demo mode drives it:
//
//   const sim = new module.Cluster(seed, Date.now(), 25);
//   sim.advance(16);                // once per animation frame
//   JSON.parse(sim.snapshot());     // what the gateway would have sent
//   sim.kill("node-c"); sim.restart("node-c"); sim.split(); sim.heal();

#include <emscripten/bind.h>

#include <string>

#include "matchmesh/sim/cluster.hpp"

namespace {

using matchmesh::sim::Cluster;
using matchmesh::sim::SimConfig;

// embind can't construct from a SimConfig, so take its fields.
Cluster* make_cluster(double seed, double unix_start_ms, double arrivals_per_second) {
    SimConfig config;
    config.seed = static_cast<std::uint64_t>(seed);
    config.unix_start_ms = unix_start_ms;
    config.arrivals_per_second = arrivals_per_second;
    return new Cluster(config);
}

std::string snapshot(const Cluster& cluster) {
    return cluster.snapshot_json();
}

}  // namespace

EMSCRIPTEN_BINDINGS(matchmesh_sim) {
    emscripten::class_<Cluster>("Cluster")
        .constructor(&make_cluster, emscripten::allow_raw_pointers())
        .function("advance", &Cluster::advance)
        .function("kill", &Cluster::kill)
        .function("restart", &Cluster::restart)
        .function("split", &Cluster::split)
        .function("heal", &Cluster::heal)
        .function("setRate", &Cluster::set_rate)
        .function("isUp", &Cluster::is_up)
        .function("snapshot", &snapshot);
}
