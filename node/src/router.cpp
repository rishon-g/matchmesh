#include "matchmesh/node/router.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace matchmesh::node {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

}  // namespace

std::map<std::string, std::string> parse_peers(std::string_view spec) {
    std::map<std::string, std::string> peers;
    while (!spec.empty()) {
        // Take the text up to the next comma as one "id=address" entry.
        const auto comma = spec.find(',');
        const std::string_view entry = trim(spec.substr(0, comma));
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        if (entry.empty()) {
            continue;  // tolerate "a=x,,b=y" and a trailing comma
        }

        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            throw std::invalid_argument("peer entry '" + std::string(entry) +
                                        "' is not of the form id=host:port");
        }
        const std::string id(trim(entry.substr(0, equals)));
        const std::string address(trim(entry.substr(equals + 1)));
        if (id.empty() || address.empty()) {
            throw std::invalid_argument("peer entry '" + std::string(entry) +
                                        "' has an empty id or address");
        }
        if (!peers.emplace(id, address).second) {
            throw std::invalid_argument("peer '" + id + "' is listed twice");
        }
    }
    return peers;
}

std::shared_ptr<grpc::Channel> make_peer_channel(const std::string& address) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    return grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
}

Router::Router(std::string self_id, const std::map<std::string, std::string>& peers,
               std::chrono::milliseconds forward_timeout)
    : self_id_(std::move(self_id)), forward_timeout_(forward_timeout) {
    if (self_id_.empty()) {
        throw std::invalid_argument("node ID must not be empty");
    }
    std::vector<std::string> everyone{self_id_};
    for (const auto& [id, address] : peers) {
        if (id == self_id_) {
            continue;
        }
        peers_.emplace(id, address);
        everyone.push_back(id);
        // Channels connect lazily, on the first call, so a peer that isn't
        // up yet is fine here.
        stubs_.emplace(id, v1::MatchmakingService::NewStub(make_peer_channel(address)));
    }
    std::ranges::sort(everyone);

    // Epoch 0: everyone configured is a member, as in Phase 4.
    auto initial = std::make_shared<Table>();
    initial->view = core::MembershipView{0, everyone};
    for (const std::string& node : everyone) {
        initial->ring.add_node(node);
    }
    table_ = std::move(initial);
}

const std::string& Router::self_id() const noexcept {
    return self_id_;
}

std::shared_ptr<const Router::Table> Router::table() const {
    const std::scoped_lock lock(mutex_);
    return table_;  // copying the shared_ptr keeps this table alive for the caller
}

RouteDecision Router::route(const core::QueueKey& queue) const {
    const auto current = table();
    return RouteDecision{*current->ring.owner(queue), current->view.epoch};
}

std::string Router::owner_of(const core::QueueKey& queue) const {
    return route(queue).owner;
}

bool Router::is_local(const core::QueueKey& queue) const {
    return owner_of(queue) == self_id_;
}

bool Router::apply_view(const core::MembershipView& view) {
    if (view.members.empty()) {
        return false;  // carries no information; never replaces a real view
    }
    // Build the new table before taking the lock, so readers are never held
    // up while 100 virtual nodes per member are hashed.
    auto next = std::make_shared<Table>();
    next->view = view;
    for (const std::string& node : view.members) {
        if (node == self_id_ || peers_.contains(node)) {
            next->ring.add_node(node);
        }
    }
    // Follow the view even if it leaves us out (the cluster thinks we're
    // dead; the coordinator re-adds us once it hears our heartbeats). Only if
    // it names nobody we can reach do we fall back to routing to ourselves.
    if (next->ring.empty()) {
        next->ring.add_node(self_id_);
    }

    const std::scoped_lock lock(mutex_);
    if (!core::is_newer(view, table_->view)) {
        return false;  // stale or already applied
    }
    table_ = std::move(next);
    return true;
}

std::uint64_t Router::epoch() const {
    return table()->view.epoch;
}

std::vector<std::string> Router::members() const {
    return table()->ring.nodes();
}

v1::MatchmakingService::StubInterface& Router::stub_for(const std::string& node_id) const {
    return *stubs_.at(node_id);
}

std::chrono::milliseconds Router::forward_timeout() const noexcept {
    return forward_timeout_;
}

const std::map<std::string, std::string>& Router::peers() const noexcept {
    return peers_;
}

}  // namespace matchmesh::node
