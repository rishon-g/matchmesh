#pragma once

// TopologyPoller: asks every node for its GetTopology a few times a second,
// merges the answers (see snapshot.hpp) and tells a listener whenever the
// merged picture changes.
//
// All nodes are asked at once, each with a short deadline, so one dead node
// costs a single timeout per round instead of delaying the others; it simply
// shows up as unreachable in that round's snapshot.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "matchmesh/v1/cluster.grpc.pb.h"

namespace matchmesh::gateway {

class TopologyPoller {
public:
    using Listener = std::function<void(const std::string& json)>;

    // `nodes` maps node ID -> host:port. `listener` is called from the
    // polling thread with each new snapshot (not with repeats).
    TopologyPoller(const std::map<std::string, std::string>& nodes, Listener listener,
                   std::chrono::milliseconds interval = std::chrono::milliseconds(500),
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(300));
    ~TopologyPoller();  // stop()

    TopologyPoller(const TopologyPoller&) = delete;
    TopologyPoller& operator=(const TopologyPoller&) = delete;

    void start();
    void stop();  // safe to call more than once

    // The most recent snapshot ("{}" before the first round finishes).
    [[nodiscard]] std::string latest() const;

    // Polls every node once and returns the merged snapshot. start() does
    // this on a timer; tests call it directly.
    std::string poll_once();

private:
    struct Node {
        std::string id;
        std::string address;
        std::unique_ptr<v1::ClusterService::Stub> stub;
    };

    void loop(std::stop_token stop);

    std::vector<Node> nodes_;
    const Listener listener_;
    const std::chrono::milliseconds interval_;
    const std::chrono::milliseconds timeout_;

    mutable std::mutex mutex_;  // guards latest_
    std::string latest_ = "{}";

    std::jthread thread_;  // last: stops before the rest goes away
};

}  // namespace matchmesh::gateway
