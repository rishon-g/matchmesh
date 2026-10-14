#include "matchmesh/core/membership.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace matchmesh::core {
namespace {

std::vector<std::string> sorted_unique(std::vector<std::string> nodes) {
    std::ranges::sort(nodes);
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    return nodes;
}

}  // namespace

std::string MembershipView::coordinator() const {
    return members.empty() ? std::string{} : members.front();
}

bool is_newer(const MembershipView& candidate, const MembershipView& current) {
    if (candidate.epoch != current.epoch) {
        return candidate.epoch > current.epoch;
    }
    // Same epoch from two different coordinators: lower coordinator ID wins.
    return candidate.coordinator() < current.coordinator();
}

Membership::Membership(std::string self, std::vector<std::string> all_nodes, TimePoint now,
                       MembershipConfig config)
    : self_(std::move(self)), all_nodes_(sorted_unique(std::move(all_nodes))), config_(config) {
    if (self_.empty() || !std::ranges::binary_search(all_nodes_, self_)) {
        throw std::invalid_argument("self must be one of the cluster's nodes");
    }
    // Everyone gets a full grace period after we start before they can be
    // suspected, so a cluster booting up doesn't declare itself dead.
    for (const std::string& node : all_nodes_) {
        last_contact_[node] = now;
    }
    view_ = MembershipView{0, all_nodes_};
}

void Membership::record_contact(const std::string& node, TimePoint now) {
    const std::scoped_lock lock(mutex_);
    if (node == self_) {
        return;
    }
    if (auto it = last_contact_.find(node); it != last_contact_.end()) {
        it->second = std::max(it->second, now);
    }
}

bool Membership::observe(const MembershipView& view) {
    const std::scoped_lock lock(mutex_);
    // A view with no members carries no information (for example a plain
    // ping that only sends an ID), so it can never replace a real one.
    if (view.members.empty() || !is_newer(view, view_)) {
        return false;
    }
    view_ = MembershipView{view.epoch, sorted_unique(view.members)};
    return true;
}

std::optional<MembershipView> Membership::tick(TimePoint now) {
    const std::scoped_lock lock(mutex_);
    std::vector<std::string> alive = alive_locked(now);
    // Only the coordinator (lowest alive node) may publish, and only when
    // something actually changed.
    if (alive.front() != self_ || alive == view_.members) {
        return std::nullopt;
    }
    view_ = MembershipView{view_.epoch + 1, std::move(alive)};
    return view_;
}

MembershipView Membership::view() const {
    const std::scoped_lock lock(mutex_);
    return view_;
}

bool Membership::is_alive(const std::string& node, TimePoint now) const {
    const std::scoped_lock lock(mutex_);
    return is_alive_locked(node, now);
}

std::vector<std::string> Membership::alive(TimePoint now) const {
    const std::scoped_lock lock(mutex_);
    return alive_locked(now);
}

std::string Membership::local_coordinator(TimePoint now) const {
    const std::scoped_lock lock(mutex_);
    return alive_locked(now).front();
}

const std::string& Membership::self() const noexcept {
    return self_;
}

const MembershipConfig& Membership::config() const noexcept {
    return config_;
}

const std::vector<std::string>& Membership::all_nodes() const noexcept {
    return all_nodes_;
}

bool Membership::is_alive_locked(const std::string& node, TimePoint now) const {
    if (node == self_) {
        return true;  // we know we're alive
    }
    const auto it = last_contact_.find(node);
    return it != last_contact_.end() && now - it->second <= config_.suspect_after;
}

std::vector<std::string> Membership::alive_locked(TimePoint now) const {
    std::vector<std::string> alive;
    for (const std::string& node : all_nodes_) {  // already sorted
        if (is_alive_locked(node, now)) {
            alive.push_back(node);
        }
    }
    return alive;  // never empty: always contains self
}

}  // namespace matchmesh::core
