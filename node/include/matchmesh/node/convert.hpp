#pragma once

// Translations between core's plain C++ types and the generated protobuf
// messages. Keeping them in one place means core never sees protobuf and the
// services stay short.

#include <google/protobuf/timestamp.pb.h>

#include <optional>

#include "matchmesh/core/replication.hpp"
#include "matchmesh/core/types.hpp"
#include "matchmesh/v1/cluster.pb.h"
#include "matchmesh/v1/common.pb.h"

namespace matchmesh::node {

void to_proto(const core::Player& in, v1::Player* out);
void to_proto(const core::QueueKey& in, v1::QueueKey* out);
void to_proto(const core::Lobby& in, v1::Lobby* out);

[[nodiscard]] core::QueueKey from_proto(const v1::QueueKey& in);
[[nodiscard]] core::Player from_proto(const v1::Player& in);
[[nodiscard]] core::Lobby from_proto(const v1::Lobby& in);

// core measures time with steady_clock, which only means something inside
// this process. Clients need wall-clock time, so convert by measuring how
// long ago `when` was and subtracting that from the current wall time.
void to_proto(core::TimePoint when, google::protobuf::Timestamp* out);
// The reverse: a wall-clock time as this process's steady_clock. Between
// nodes on one host (or NTP-synced hosts) wait times survive the trip.
[[nodiscard]] core::TimePoint from_proto(const google::protobuf::Timestamp& in);

// Replication messages. These fill in the queue, sequence and op of a
// ReplicateRequest; the caller adds the sender and epoch.
void to_proto(const core::ShardChange& in, v1::ReplicateRequest* out);
void to_proto(const core::ShardState& in, v1::ReplicateRequest* out);
// The change a request carries, or nothing if it carries a snapshot (or
// nothing at all).
[[nodiscard]] std::optional<core::ShardChange> change_from_proto(const v1::ReplicateRequest& in);
// The snapshot a request carries. Call only when it has one.
[[nodiscard]] core::ShardState state_from_proto(const v1::ReplicateRequest& in);

}  // namespace matchmesh::node
