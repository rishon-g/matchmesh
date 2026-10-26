#pragma once

#include <string_view>

namespace matchmesh::core {

// Returns the project version, e.g. "0.1.0", as set by project() in the
// top-level CMakeLists.txt.
std::string_view version() noexcept;

}  // namespace matchmesh::core
