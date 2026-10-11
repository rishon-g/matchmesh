#pragma once

// Translations between core's plain C++ types and the generated protobuf
// messages. Keeping them in one place means core never sees protobuf and the
// services stay short.

#include <google/protobuf/timestamp.pb.h>

#include "matchmesh/core/types.hpp"
#include "matchmesh/v1/common.pb.h"

namespace matchmesh::node {

void to_proto(const core::Player& in, v1::Player* out);
void to_proto(const core::QueueKey& in, v1::QueueKey* out);
void to_proto(const core::Lobby& in, v1::Lobby* out);

[[nodiscard]] core::QueueKey from_proto(const v1::QueueKey& in);

// core measures time with steady_clock, which only means something inside
// this process. Clients need wall-clock time, so convert by measuring how
// long ago `when` was and subtracting that from the current wall time.
void to_proto(core::TimePoint when, google::protobuf::Timestamp* out);

}  // namespace matchmesh::node
