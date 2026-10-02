#pragma once

#include <string_view>

namespace matchmesh::core {

// Returns the project version, e.g. "0.1.0".
// A tiny placeholder so the build has real code to compile and link.
std::string_view version() noexcept;

}  // namespace matchmesh::core
