#include "matchmesh/node/router.hpp"

#include <stdexcept>
#include <utility>

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

Router::Router(std::string self_id, const std::map<std::string, std::string>& peers,
               std::chrono::milliseconds forward_timeout)
    : self_id_(std::move(self_id)), forward_timeout_(forward_timeout) {
    ring_.add_node(self_id_);  // throws if the ID is empty
    for (const auto& [id, address] : peers) {
        if (id == self_id_) {
            continue;
        }
        peers_.emplace(id, address);
        ring_.add_node(id);
        // Channels connect lazily, on the first call, so a peer that isn't
        // up yet is fine here.
        stubs_.emplace(id, v1::MatchmakingService::NewStub(
                               grpc::CreateChannel(address, grpc::InsecureChannelCredentials())));
    }
}

const std::string& Router::self_id() const noexcept {
    return self_id_;
}

std::string Router::owner_of(const core::QueueKey& queue) const {
    return *ring_.owner(queue);  // the ring always contains this node
}

bool Router::is_local(const core::QueueKey& queue) const {
    return owner_of(queue) == self_id_;
}

v1::MatchmakingService::StubInterface& Router::stub_for(const std::string& node_id) const {
    return *stubs_.at(node_id);
}

std::chrono::milliseconds Router::forward_timeout() const noexcept {
    return forward_timeout_;
}

const core::HashRing& Router::ring() const noexcept {
    return ring_;
}

const std::map<std::string, std::string>& Router::peers() const noexcept {
    return peers_;
}

}  // namespace matchmesh::node
