#include "matchmesh/sim/cluster.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <utility>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/hash_ring.hpp"
#include "matchmesh/core/membership.hpp"
#include "matchmesh/core/placement.hpp"
#include "matchmesh/core/shard_coordinator.hpp"
#include "matchmesh/core/types.hpp"
#include "json.hpp"

namespace matchmesh::sim {

namespace {

// The real node's timings (node/include/matchmesh/node/cluster_manager.hpp,
// shard_manager.hpp, core::EngineConfig).
constexpr double kStepMs = 10.0;
constexpr double kHeartbeatMs = 500.0;
constexpr double kCheckMs = 100.0;
constexpr double kPassMs = 100.0;
constexpr double kMatchMs = 50.0;
// Client retries, like the load generator: 50 ms doubling to 800 ms.
constexpr double kFirstRetryMs = 50.0;
constexpr double kMaxRetryMs = 800.0;
constexpr std::size_t kMaxEvents = 60;
// How often the invariants are re-checked, to log the moment one breaks.
constexpr double kAuditMs = 250.0;

core::TimePoint at(double ms) {
    return core::TimePoint{} + std::chrono::duration_cast<core::Clock::duration>(
                                   std::chrono::duration<double, std::milli>(ms));
}

double ms_of(core::TimePoint time) {
    return std::chrono::duration<double, std::milli>(time.time_since_epoch()).count();
}

std::string join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

std::string lobby_id_prefix(const std::string& node, std::uint64_t incarnation) {
    char run[9];
    std::snprintf(run, sizeof run, "%08x", static_cast<unsigned>(incarnation & 0xffffffffU));
    return node + "-" + run;
}

struct Heartbeat {
    std::string node;
    std::uint64_t incarnation = 0;
    core::MembershipView view;
    std::optional<std::uint64_t> synced_epoch;
};

struct Pending {
    core::Player player;
    std::string region;
    std::string mode;
    std::string request_id;
    double next_try_ms = 0.0;
    int attempts = 0;
};

}  // namespace

// ---- One simulated node process ----

class Node final : public core::PlacementSource, public core::ReplicaTransport {
public:
    Node(Cluster::Impl& sim, std::string id, std::uint64_t incarnation, double now_ms);

    // core::PlacementSource, answered like node::Router.
    [[nodiscard]] const std::string& self_id() const override { return id_; }
    [[nodiscard]] core::Placement placement(const core::QueueKey& queue) const override {
        return core::place(ring_, view_.epoch, queue);
    }
    [[nodiscard]] core::MembershipView view() const override { return view_; }

    // core::ReplicaTransport over the simulated network.
    bool send_change(const std::string& target, std::uint64_t epoch,
                     const core::ShardChange& change) override;
    bool send_snapshot(const std::string& target, std::uint64_t epoch,
                       const core::ShardState& state) override;

    void step(double now_ms);
    [[nodiscard]] Heartbeat describe() const {
        return Heartbeat{id_, incarnation_, membership_.view(), coordinator_.synced_epoch()};
    }
    void heard_from(const Heartbeat& beat, double now_ms);

    [[nodiscard]] core::MatchmakingEngine& engine() { return engine_; }
    [[nodiscard]] const core::MatchmakingEngine& engine() const { return engine_; }
    [[nodiscard]] core::ShardCoordinator& coordinator() { return coordinator_; }
    [[nodiscard]] const core::Membership& membership() const { return membership_; }
    // Whether this node serves `queue` to clients right now.
    [[nodiscard]] bool serving(const core::QueueKey& queue) const;

private:
    void adopt(const core::MembershipView& view, const std::string& reason);
    void beat(double now_ms);

    Cluster::Impl& sim_;
    const std::string id_;
    const std::uint64_t incarnation_;
    core::Membership membership_;
    core::MembershipView view_;  // the one routing follows, like node::Router
    core::HashRing ring_;
    double now_ms_;
    core::BackupReplicator backups_;
    core::MatchmakingEngine engine_;
    core::ShardCoordinator coordinator_;

    double next_beat_ms_;
    double next_check_ms_;
    double next_pass_ms_;
    double next_match_ms_;
    bool pass_now_ = true;
    std::vector<std::string> alive_;  // our own opinion, for logging changes
};

// ---- The whole cluster ----

class Cluster::Impl {
public:
    explicit Impl(SimConfig config)
        : config_(std::move(config)), rng_(config_.seed) {
        std::ranges::sort(config_.nodes);
        for (const std::string& id : config_.nodes) {
            start(id);
        }
        schedule_next_arrival();
    }

    // ---- The network ----

    [[nodiscard]] Node* node(const std::string& id) const {
        const auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : it->second.get();
    }

    // During a split: true for the right-hand side (every node but the first two).
    [[nodiscard]] bool side_of(const std::string& id) const {
        const std::vector<std::string> left = left_side();
        return split_ && std::ranges::find(left, id) == left.end();
    }

    [[nodiscard]] bool link(const std::string& from, const std::string& to) const {
        return node(from) != nullptr && node(to) != nullptr && side_of(from) == side_of(to);
    }

    [[nodiscard]] std::vector<std::string> left_side() const {
        return {config_.nodes.begin(), config_.nodes.begin() + 2};
    }

    // Delivers a replication message; true if the receiver accepted it.
    template <typename Message>
    bool deliver(const std::string& from, const std::string& to, std::uint64_t epoch,
                 const Message& message) {
        if (!link(from, to)) {
            return false;
        }
        return node(to)->coordinator().receive(from, epoch, message).accepted;
    }

    // One heartbeat round trip, if the two can talk.
    void heartbeat(Node& from, const std::string& to) {
        if (!link(from.self_id(), to)) {
            return;
        }
        Node& peer = *node(to);
        peer.heard_from(from.describe(), now_ms_);
        from.heard_from(peer.describe(), now_ms_);
    }

    // ---- Processes ----

    void start(const std::string& id) {
        std::uint64_t incarnation = 0;
        while (incarnation == 0) {
            incarnation = rng_();
        }
        nodes_[id] = std::make_unique<Node>(*this, id, incarnation, now_ms_);
    }

    // ---- Time ----

    void advance(double ms) {
        const double until = now_ms_ + std::max(0.0, ms);
        while (now_ms_ + kStepMs <= until + 1e-9) {
            now_ms_ += kStepMs;
            step();
        }
    }

    void step() {
        arrive();
        // A copy of the IDs: a step can't add or remove nodes, but stay safe.
        for (const std::string& id : config_.nodes) {
            if (Node* n = node(id)) {
                n->step(now_ms_);
            }
        }
        retry_clients();
        if (now_ms_ >= next_audit_ms_) {
            next_audit_ms_ = now_ms_ + kAuditMs;
            audit();
        }
    }

    Invariants check() const;

    // Logs the moment players go missing or land in two lobbies.
    void audit() {
        const Invariants now = check();
        if (now.lost > reported_lost_) {
            log("warn", std::to_string(now.lost - reported_lost_) +
                            " acknowledged player(s) are now held by no live node: lost");
        }
        if (now.in_two_lobbies > reported_two_) {
            log("warn", std::to_string(now.in_two_lobbies - reported_two_) +
                            " player(s) were put in a second lobby by the other side of the split");
        }
        reported_lost_ = now.lost;
        reported_two_ = now.in_two_lobbies;
    }

    // ---- Clients ----

    void schedule_next_arrival() {
        if (config_.arrivals_per_second <= 0.0) {
            next_arrival_ms_ = -1.0;
            return;
        }
        std::exponential_distribution<double> gap(config_.arrivals_per_second / 1000.0);
        next_arrival_ms_ = now_ms_ + gap(rng_);
    }

    void arrive() {
        static const std::vector<std::pair<std::string, double>> kRegions = {
            {"us-west", 0.30}, {"us-east", 0.30}, {"eu", 0.25}, {"asia", 0.15}};
        static const std::vector<std::pair<std::string, double>> kModes = {{"ranked", 0.60},
                                                                           {"casual", 0.40}};
        while (next_arrival_ms_ >= 0.0 && next_arrival_ms_ <= now_ms_) {
            const std::string id = "p" + std::to_string(++players_);
            std::normal_distribution<double> rating(1500.0, 300.0);
            Pending pending;
            pending.player = core::Player{
                id, static_cast<std::int32_t>(std::clamp(std::lround(rating(rng_)), 0L, 3000L))};
            pending.region = pick(kRegions);
            pending.mode = pick(kModes);
            pending.request_id = "req-" + id;
            pending.next_try_ms = now_ms_;
            pending_.push_back(std::move(pending));
            schedule_next_arrival();
        }
    }

    std::string pick(const std::vector<std::pair<std::string, double>>& weighted) {
        double total = 0.0;
        for (const auto& [name, weight] : weighted) {
            total += weight;
        }
        double roll = std::uniform_real_distribution<double>(0.0, total)(rng_);
        for (const auto& [name, weight] : weighted) {
            roll -= weight;
            if (roll <= 0.0) {
                return name;
            }
        }
        return weighted.back().first;
    }

    void retry_clients() {
        std::deque<Pending> waiting;
        while (!pending_.empty()) {
            Pending pending = std::move(pending_.front());
            pending_.pop_front();
            if (pending.next_try_ms > now_ms_) {
                waiting.push_back(std::move(pending));
                continue;
            }
            if (try_enqueue(pending)) {
                acked_.emplace(pending.player.id, queue_of(pending));
                continue;
            }
            if (pending.attempts > 0) {
                ++retries_;
            }
            ++pending.attempts;
            const double backoff =
                std::min(kMaxRetryMs, kFirstRetryMs * std::pow(2.0, pending.attempts - 1));
            pending.next_try_ms = now_ms_ + backoff;
            waiting.push_back(std::move(pending));
        }
        pending_ = std::move(waiting);
    }

    static core::QueueKey queue_of(const Pending& pending) {
        return core::QueueKey{pending.region, pending.mode,
                              core::skill_bracket_for(pending.player.rating)};
    }

    // One attempt, as node::MatchmakingService handles it: the node the
    // client reached forwards to the queue's owner; the owner admits and
    // enqueues (replicating to its backup first).
    bool try_enqueue(const Pending& pending) {
        std::vector<std::string> up;
        for (const std::string& id : config_.nodes) {
            if (node(id) != nullptr) {
                up.push_back(id);
            }
        }
        if (up.empty()) {
            return false;
        }
        const std::string& entry =
            up[std::uniform_int_distribution<std::size_t>(0, up.size() - 1)(rng_)];
        const core::QueueKey queue = queue_of(pending);
        const std::string owner = node(entry)->placement(queue).primary;
        if (owner != entry && !link(entry, owner)) {
            return false;  // the owner is unreachable from here: UNAVAILABLE
        }
        Node& target = *node(owner);
        if (target.placement(queue).primary != owner || target.coordinator().admit(queue)) {
            return false;  // moving, or not settled yet: UNAVAILABLE, retry
        }
        const core::EnqueueResult result =
            target.engine()
                .enqueue(pending.player, pending.region, pending.mode, pending.request_id)
                .result;
        return result == core::EnqueueResult::kAdded ||
               result == core::EnqueueResult::kAlreadyDone;
    }

    // ---- Events ----

    void log(std::string kind, std::string text) {
        events_.push_back(Event{++event_seq_, now_ms_, std::move(kind), std::move(text)});
        if (events_.size() > kMaxEvents) {
            events_.pop_front();
        }
    }

    // Every epoch is announced once, by whoever publishes it.
    void published(const core::MembershipView& view, const std::string& by) {
        if (announced_.insert({view.epoch, by}).second) {
            log("view", "epoch " + std::to_string(view.epoch) + " published by " + by +
                            ": members [" + join(view.members) + "]");
        }
    }

    // ---- State ----

    SimConfig config_;
    std::mt19937_64 rng_;
    double now_ms_ = 0.0;
    std::map<std::string, std::unique_ptr<Node>> nodes_;  // only live processes
    bool split_ = false;

    double next_arrival_ms_ = 0.0;
    std::uint64_t players_ = 0;
    std::deque<Pending> pending_;
    std::unordered_map<std::string, core::QueueKey> acked_;  // player -> their queue
    std::size_t retries_ = 0;

    double next_audit_ms_ = 0.0;
    std::size_t reported_lost_ = 0;
    std::size_t reported_two_ = 0;

    std::deque<Event> events_;
    std::uint64_t event_seq_ = 0;
    std::set<std::pair<std::uint64_t, std::string>> announced_;
};

// ---- Node ----

Node::Node(Cluster::Impl& sim, std::string id, std::uint64_t incarnation, double now_ms)
    : sim_(sim),
      id_(std::move(id)),
      incarnation_(incarnation),
      membership_(id_, sim.config_.nodes, at(now_ms), core::MembershipConfig{}, incarnation),
      view_(membership_.view()),
      now_ms_(now_ms),
      backups_(*this, *this),
      engine_(lobby_id_prefix(id_, incarnation),
              core::EngineConfig{.worker_count = 0},
              [this] { return at(now_ms_); }, &backups_),
      coordinator_(engine_, *this, *this, incarnation),
      next_beat_ms_(now_ms),
      next_check_ms_(now_ms),
      next_pass_ms_(now_ms),
      next_match_ms_(now_ms),
      alive_(membership_.alive(at(now_ms))) {
    for (const std::string& member : view_.members) {
        ring_.add_node(member);
    }
}

bool Node::send_change(const std::string& target, std::uint64_t epoch,
                       const core::ShardChange& change) {
    return sim_.deliver(id_, target, epoch, change);
}

bool Node::send_snapshot(const std::string& target, std::uint64_t epoch,
                         const core::ShardState& state) {
    return sim_.deliver(id_, target, epoch, state);
}

bool Node::serving(const core::QueueKey& queue) const {
    const auto info = engine_.shard_info(queue);
    return info && info->role == core::ShardRole::kPrimary && placement(queue).primary == id_;
}

void Node::step(double now_ms) {
    now_ms_ = now_ms;
    if (now_ms >= next_beat_ms_) {
        beat(now_ms);
    }
    if (now_ms >= next_check_ms_) {
        next_check_ms_ = now_ms + kCheckMs;
        const std::vector<std::string> alive = membership_.alive(at(now_ms));
        // Only the coordinator's opinion is worth showing; everyone else's
        // is the same news, five times over.
        if (membership_.local_coordinator(at(now_ms)) == id_) {
            for (const std::string& node : alive_) {
                if (!std::ranges::binary_search(alive, node)) {
                    sim_.log("detect", id_ + " stopped hearing from " + node + " (1.5 s silent)");
                }
            }
        }
        alive_ = alive;
        if (const auto published = membership_.tick(at(now_ms))) {
            sim_.published(*published, id_);
            adopt(*published, "published");
        }
    }
    if (pass_now_ || now_ms >= next_pass_ms_) {
        pass_now_ = false;
        next_pass_ms_ = now_ms + kPassMs;
        const core::CoordinatorPass pass = coordinator_.run_pass();
        if (pass.finished_hand_over) {
            if (pass.copies_sent > 0) {
                sim_.log("handover", id_ + " handed " + std::to_string(pass.copies_sent) +
                                         " queue(s) to their new owners");
            }
            next_beat_ms_ = now_ms;  // tell everyone right away (on_synced)
        }
        if (pass.promoted > 0) {
            sim_.log("promote", id_ + " now serves " + std::to_string(pass.promoted) +
                                    " more queue(s) as primary (epoch " +
                                    std::to_string(pass.epoch) + ")");
        }
    }
    if (now_ms >= next_match_ms_) {
        next_match_ms_ = now_ms + kMatchMs;
        (void)engine_.run_match_pass(at(now_ms));
    }
}

void Node::beat(double now_ms) {
    next_beat_ms_ = now_ms + kHeartbeatMs;
    for (const std::string& peer : sim_.config_.nodes) {
        if (peer != id_) {
            sim_.heartbeat(*this, peer);
        }
    }
}

void Node::heard_from(const Heartbeat& beat, double now_ms) {
    membership_.record_contact(beat.node, at(now_ms), beat.incarnation);
    coordinator_.record_progress(beat.node, beat.synced_epoch);
    if (membership_.observe(beat.view)) {
        adopt(beat.view, "learned from " + beat.node);
    }
}

void Node::adopt(const core::MembershipView& view, const std::string& /*reason*/) {
    if (!core::is_newer(view, view_)) {
        return;
    }
    view_ = view;
    ring_ = core::HashRing{};
    for (const std::string& member : view_.members) {
        ring_.add_node(member);
    }
    pass_now_ = true;               // ShardManager::on_view_changed
    next_beat_ms_ = now_ms_;        // ClusterManager::nudge
}

// ---- Cluster: the public face ----

Cluster::Cluster(SimConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Cluster::~Cluster() = default;

void Cluster::advance(double ms) {
    impl_->advance(ms);
}

void Cluster::kill(const std::string& node) {
    if (impl_->nodes_.erase(node) > 0) {
        impl_->log("action", node + " killed (its memory is gone)");
    }
}

void Cluster::restart(const std::string& node) {
    if (impl_->node(node) != nullptr ||
        std::ranges::find(impl_->config_.nodes, node) == impl_->config_.nodes.end()) {
        return;
    }
    impl_->start(node);
    impl_->log("action", node + " restarted: a new, empty process");
}

void Cluster::split() {
    if (!impl_->split_) {
        impl_->split_ = true;
        std::vector<std::string> right(impl_->config_.nodes.begin() + 2, impl_->config_.nodes.end());
        impl_->log("action", "network split: [" + join(impl_->left_side()) + "] | [" +
                                 join(right) + "]");
    }
}

void Cluster::heal() {
    if (impl_->split_) {
        impl_->split_ = false;
        impl_->log("action", "network healed");
    }
}

void Cluster::set_rate(double arrivals_per_second) {
    impl_->config_.arrivals_per_second = std::max(0.0, arrivals_per_second);
    impl_->schedule_next_arrival();
}

double Cluster::now_ms() const {
    return impl_->now_ms_;
}

bool Cluster::is_up(const std::string& node) const {
    return impl_->node(node) != nullptr;
}

bool Cluster::is_split() const {
    return impl_->split_;
}

double Cluster::rate() const {
    return impl_->config_.arrivals_per_second;
}

const std::vector<std::string>& Cluster::node_ids() const {
    return impl_->config_.nodes;
}

std::vector<Event> Cluster::events() const {
    return {impl_->events_.begin(), impl_->events_.end()};
}

namespace {

// The newest view any live node routes by.
const Node* newest(const Cluster::Impl& sim) {
    const Node* best = nullptr;
    for (const auto& [id, node] : sim.nodes_) {
        if (best == nullptr || core::is_newer(node->view(), best->view())) {
            best = node.get();
        }
    }
    return best;
}

}  // namespace

std::uint64_t Cluster::epoch() const {
    const Node* best = newest(*impl_);
    return best == nullptr ? 0 : best->view().epoch;
}

std::vector<std::string> Cluster::members() const {
    const Node* best = newest(*impl_);
    return best == nullptr ? std::vector<std::string>{} : best->view().members;
}

std::string Cluster::serving_node(const std::string& queue_name) const {
    for (const auto& [id, node] : impl_->nodes_) {
        for (const core::QueueKey& queue : node->engine().shards()) {
            if (core::to_string(queue) == queue_name && node->serving(queue)) {
                return id;
            }
        }
    }
    return "";
}

Invariants Cluster::invariants() const {
    return impl_->check();
}

Invariants Cluster::Impl::check() const {
    Invariants result;
    result.acknowledged = acked_.size();
    result.retries = retries_;
    // Each acknowledged player, looked up in every live node's copy of its
    // queue (primaries and backups alike).
    std::set<std::string> lobbies;
    for (const auto& [player, queue] : acked_) {
        bool present = false;
        lobbies.clear();
        for (const auto& [id, node] : nodes_) {
            const core::PlayerLookup found = node->engine().lookup(queue, player);
            if (found.status == core::PlayerStatus::kQueued) {
                present = true;
            } else if (found.status == core::PlayerStatus::kInLobby && found.lobby) {
                present = true;
                lobbies.insert(found.lobby->lobby_id);
            }
        }
        if (!present) {
            ++result.lost;
        } else if (lobbies.empty()) {
            ++result.queued;
        } else {
            ++result.in_lobby;
            if (lobbies.size() > 1) {
                ++result.in_two_lobbies;
            }
        }
    }
    std::map<std::string, int> servers;  // queue -> live nodes serving it
    for (const auto& [id, node] : nodes_) {
        for (const core::QueueKey& queue : node->engine().shards()) {
            if (node->serving(queue)) {
                ++servers[core::to_string(queue)];
            }
        }
    }
    for (const auto& [queue, count] : servers) {
        if (count > 1) {
            ++result.contested_queues;
        }
    }
    return result;
}

std::string Cluster::snapshot_json(std::size_t max_lobbies) const {
    const Impl& sim = *impl_;
    const Node* best = newest(sim);
    const core::MembershipView view = best != nullptr ? best->view() : core::MembershipView{};
    const std::set<std::string> members(view.members.begin(), view.members.end());
    core::HashRing ring;
    for (const std::string& member : view.members) {
        ring.add_node(member);
    }

    // ---- Shards: each queue once, as the gateway merges them ----
    struct Shard {
        core::QueueKey queue;
        std::string primary;
        std::string backup;
        std::size_t queued = 0;
        int servers = 0;
    };
    std::map<std::string, Shard> shards;
    for (const auto& [id, node] : sim.nodes_) {
        for (const core::ServedShard& served : node->coordinator().served_shards()) {
            const std::string name = core::to_string(served.queue);
            Shard candidate{served.queue, id, served.backup.value_or(""), served.queued, 0};
            const auto [it, inserted] = shards.try_emplace(name, candidate);
            const int servers = it->second.servers + 1;
            if (!inserted && !members.empty() && ring.owner(served.queue) == id) {
                it->second = candidate;  // two claim it: trust the newest view's ring
            }
            it->second.servers = servers;
        }
    }
    std::map<std::string, std::pair<std::size_t, std::size_t>> per_node;  // queues, players
    std::size_t queued_total = 0;
    std::vector<std::pair<double, const Shard*>> ordered;
    for (const auto& [name, shard] : shards) {
        ordered.emplace_back(
            static_cast<double>(core::HashRing::position(shard.queue)) / 18446744073709551616.0,
            &shard);
        auto& [queues, players] = per_node[shard.primary];
        ++queues;
        players += shard.queued;
        queued_total += shard.queued;
    }
    std::ranges::sort(ordered, {}, &std::pair<double, const Shard*>::first);

    // ---- Lobbies: newest first, each once ----
    std::vector<std::pair<core::Lobby, std::string>> lobbies;
    std::set<std::string> seen_lobbies;
    for (const auto& [id, node] : sim.nodes_) {
        for (const core::Lobby& lobby : node->engine().recent_lobbies()) {
            if (seen_lobbies.insert(lobby.lobby_id).second) {
                lobbies.emplace_back(lobby, id);
            }
        }
    }
    std::ranges::sort(lobbies, [](const auto& a, const auto& b) {
        return a.first.formed_at != b.first.formed_at ? a.first.formed_at > b.first.formed_at
                                                      : a.first.lobby_id > b.first.lobby_id;
    });
    if (lobbies.size() > max_lobbies) {
        lobbies.resize(max_lobbies);
    }

    Json out;
    out.begin_object();
    out.key("epoch").number(view.epoch);
    out.key("coordinator").string(best != nullptr ? view.coordinator() : "");
    out.key("members").number(members.size());
    out.key("queued_players").number(queued_total);

    out.key("nodes").begin_array();
    for (const std::string& id : sim.config_.nodes) {
        const Node* node = sim.node(id);
        const auto counts = per_node.find(id);
        out.begin_object();
        out.key("id").string(id);
        out.key("address").string(node != nullptr ? "simulated" : "down");
        out.key("reachable").boolean(node != nullptr);
        out.key("member").boolean(members.contains(id));
        out.key("queues").number(counts == per_node.end() ? 0 : counts->second.first);
        out.key("queued").number(counts == per_node.end() ? 0 : counts->second.second);
        // Extras for the demo: this node's own view of the cluster.
        out.key("epoch").number(node != nullptr ? node->view().epoch : 0);
        out.key("side").number(sim.split_ ? (sim.side_of(id) ? 2 : 1) : 0);
        out.end_object();
    }
    out.end_array();

    out.key("ring").begin_array();
    for (const core::HashRing::Point& point : ring.points()) {
        out.begin_object();
        out.key("node").string(point.node);
        out.key("at").number(static_cast<double>(point.position) / 18446744073709551616.0);
        out.end_object();
    }
    out.end_array();

    out.key("shards").begin_array();
    for (const auto& [position, shard] : ordered) {
        out.begin_object();
        out.key("queue").string(core::to_string(shard->queue));
        out.key("region").string(shard->queue.region);
        out.key("mode").string(shard->queue.game_mode);
        out.key("bracket").number(shard->queue.skill_bracket);
        out.key("primary").string(shard->primary);
        out.key("backup").string(shard->backup);
        out.key("queued").number(shard->queued);
        out.key("at").number(position);
        out.key("contested").boolean(shard->servers > 1);
        out.end_object();
    }
    out.end_array();

    out.key("lobbies").begin_array();
    for (const auto& [lobby, node] : lobbies) {
        out.begin_object();
        out.key("id").string(lobby.lobby_id);
        out.key("queue").string(core::to_string(lobby.queue));
        out.key("node").string(node);
        out.key("formed_at").number(std::llround(sim.config_.unix_start_ms + ms_of(lobby.formed_at)));
        out.key("players").begin_array();
        for (const core::Player& player : lobby.players) {
            out.begin_object();
            out.key("id").string(player.id);
            out.key("rating").number(player.rating);
            out.end_object();
        }
        out.end_array();
        out.end_object();
    }
    out.end_array();

    // ---- The demo's own section ----
    const Invariants checks = invariants();
    out.key("sim").begin_object();
    out.key("now").number(std::llround(sim.config_.unix_start_ms + sim.now_ms_));
    out.key("elapsed_ms").number(std::llround(sim.now_ms_));
    out.key("rate").number(sim.config_.arrivals_per_second);
    out.key("split").boolean(sim.split_);
    out.key("waiting_clients").number(sim.pending_.size());
    out.key("invariants").begin_object();
    out.key("acknowledged").number(checks.acknowledged);
    out.key("in_lobby").number(checks.in_lobby);
    out.key("queued").number(checks.queued);
    out.key("lost").number(checks.lost);
    out.key("in_two_lobbies").number(checks.in_two_lobbies);
    out.key("contested_queues").number(checks.contested_queues);
    out.key("retries").number(checks.retries);
    out.end_object();
    out.key("events").begin_array();
    for (const Event& event : sim.events_) {
        out.begin_object();
        out.key("seq").number(event.seq);
        out.key("at").number(std::llround(sim.config_.unix_start_ms + event.at_ms));
        out.key("kind").string(event.kind);
        out.key("text").string(event.text);
        out.end_object();
    }
    out.end_array();
    out.end_object();

    out.end_object();
    return out.str();
}

}  // namespace matchmesh::sim
