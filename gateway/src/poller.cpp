#include "matchmesh/gateway/poller.hpp"

#include <condition_variable>
#include <cstddef>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "matchmesh/gateway/snapshot.hpp"
#include "matchmesh/node/router.hpp"

namespace matchmesh::gateway {

TopologyPoller::TopologyPoller(const std::map<std::string, std::string>& nodes, Listener listener,
                               std::chrono::milliseconds interval,
                               std::chrono::milliseconds timeout)
    : listener_(std::move(listener)), interval_(interval), timeout_(timeout) {
    for (const auto& [id, address] : nodes) {
        nodes_.push_back(Node{id, address,
                              v1::ClusterService::NewStub(node::make_peer_channel(address))});
    }
}

TopologyPoller::~TopologyPoller() {
    stop();
}

void TopologyPoller::start() {
    if (!thread_.joinable()) {
        thread_ = std::jthread([this](std::stop_token stop) { loop(std::move(stop)); });
    }
}

void TopologyPoller::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

std::string TopologyPoller::latest() const {
    const std::scoped_lock lock(mutex_);
    return latest_;
}

std::string TopologyPoller::poll_once() {
    // One call per node, all in flight at once (gRPC's callback API); then
    // wait for every one to finish or hit its deadline.
    struct Call {
        grpc::ClientContext context;
        v1::GetTopologyResponse response;
        grpc::Status status;
    };
    std::vector<std::unique_ptr<Call>> calls;
    std::mutex done_mutex;
    std::condition_variable done;
    std::size_t remaining = nodes_.size();
    const v1::GetTopologyRequest request;
    for (Node& node : nodes_) {
        auto& call = calls.emplace_back(std::make_unique<Call>());
        call->context.set_deadline(std::chrono::system_clock::now() + timeout_);
        node.stub->async()->GetTopology(&call->context, &request, &call->response,
                                        [&, raw = call.get()](grpc::Status status) {
                                            raw->status = std::move(status);
                                            const std::scoped_lock lock(done_mutex);
                                            if (--remaining == 0) {
                                                done.notify_one();
                                            }
                                        });
    }
    {
        std::unique_lock lock(done_mutex);
        done.wait(lock, [&] { return remaining == 0; });
    }

    std::vector<NodePoll> polls;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        NodePoll poll{nodes_[i].id, nodes_[i].address, std::nullopt};
        if (calls[i]->status.ok()) {
            poll.topology = std::move(calls[i]->response);
        }
        polls.push_back(std::move(poll));
    }
    std::string snapshot = build_snapshot(polls);
    {
        const std::scoped_lock lock(mutex_);
        if (snapshot == latest_) {
            return snapshot;  // nothing changed; don't bother the listener
        }
        latest_ = snapshot;
    }
    if (listener_) {
        listener_(snapshot);
    }
    return snapshot;
}

void TopologyPoller::loop(std::stop_token stop) {
    std::mutex sleep_mutex;
    std::condition_variable_any sleeper;
    while (!stop.stop_requested()) {
        const auto next = std::chrono::steady_clock::now() + interval_;
        poll_once();
        std::unique_lock lock(sleep_mutex);
        sleeper.wait_until(lock, stop, next, [] { return false; });
    }
}

}  // namespace matchmesh::gateway
