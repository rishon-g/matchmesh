#include "matchmesh/node/identity.hpp"

#include <chrono>
#include <cstdio>
#include <random>

namespace matchmesh::node {

std::uint64_t new_incarnation() {
    std::random_device device;
    std::uint64_t value = 0;
    while (value == 0) {
        value = (std::uint64_t{device()} << 32) ^ std::uint64_t{device()} ^
                static_cast<std::uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
    }
    return value;
}

std::string lobby_id_prefix(const std::string& node_id, std::uint64_t incarnation) {
    // The low 32 bits are plenty to tell one node's runs apart.
    char run[9];
    std::snprintf(run, sizeof run, "%08x", static_cast<unsigned>(incarnation & 0xffffffffU));
    return node_id + "-" + run;
}

}  // namespace matchmesh::node
