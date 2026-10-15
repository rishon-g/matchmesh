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

std::uint64_t MembershipView::incarnation_of(const std::string& node) const {
    const auto it = incarnations.find(node);
    return it == incarnations.end() ? 0 : it->second;
}

bool MembershipView::lists(const std::string& node, std::uint64_t incarnation) const {
    return std::ranges::binary_search(members, node) && incarnation_of(node) == incarnation;
}

bool is_newer(const MembershipView& candidate, const MembershipView& current) {
    if (candidate.epoch != current.epoch) {
        return candidate.epoch > current.epoch;
    }
    // Same epoch from two different coordinators: lower coordinator ID wins.
    return candidate.coordinator() < current.coordinator();
}

Membership::Membership(std::string self, std::vector<std::string> all_nodes, TimePoint now,
                       MembershipConfig config, std::uint64_t incarnation)
    : self_(std::move(self)),
      all_nodes_(sorted_unique(std::move(all_nodes))),
      config_(config),
      incarnation_(incarnation) {
    if (self_.empty() || !std::ranges::binary_search(all_nodes_, self_)) {
        throw std::invalid_argument("self must be one of the cluster's nodes");
    }
    // Everyone gets a full grace period after we start before they can be
    // suspected, so a cluster booting up doesn't declare itself dead.
    for (const std::string& node : all_nodes_) {
        last_contact_[node] = now;
    }
    incarnation_of_[self_] = incarnation_;
    // Epoch 0 records no incarnations, so with real ones in use the first
    // coordinator publishes epoch 1 as soon as it has heard from everyone.
    view_ = MembershipView{0, all_nodes_, {}};
}

void Membership::record_contact(const std::string& node, TimePoint now,
                                std::uint64_t incarnation) {
    const std::scoped_lock lock(mutex_);
    if (node == self_) {
        return;
    }
    if (auto it = last_contact_.find(node); it != last_contact_.end()) {
        it->second = std::max(it->second, now);
        incarnation_of_[node] = incarnation;
    }
}

bool Membership::observe(const MembershipView& view) {
    const std::scoped_lock lock(mutex_);
    // A view with no members carries no information (for example a plain
    // ping that only sends an ID), so it can never replace a real one.
    if (view.members.empty() || !is_newer(view, view_)) {
        return false;
    }
    view_ = MembershipView{view.epoch, sorted_unique(view.members), {}};
    for (const auto& [node, incarnation] : view.incarnations) {
        if (incarnation != 0) {
            view_.incarnations.emplace(node, incarnation);
        }
    }
    return true;
}

std::optional<MembershipView> Membership::tick(TimePoint now) {
    const std::scoped_lock lock(mutex_);
    std::vector<std::string> alive = alive_locked(now);
    // Only the coordinator (lowest alive node) may publish, and only when
    // something actually changed.
    if (alive.front() != self_) {
        return std::nullopt;
    }
    std::map<std::string, std::uint64_t> incarnations;
    for (const std::string& node : alive) {
        const auto known = incarnation_of_.find(node);
        if (known == incarnation_of_.end()) {
            // Still in the startup grace period and not heard from yet: wait
            // rather than publish a view that doesn't know who it lists.
            return std::nullopt;
        }
        if (known->second != 0) {
            incarnations.emplace(node, known->second);
        }
    }
    if (alive == view_.members && incarnations == view_.incarnations) {
        return std::nullopt;
    }
    view_ = MembershipView{view_.epoch + 1, std::move(alive), std::move(incarnations)};
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

std::uint64_t Membership::incarnation() const noexcept {
    return incarnation_;
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
