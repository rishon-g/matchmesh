#include "matchmesh/core/version.hpp"

namespace matchmesh::core {

std::string_view version() noexcept {
    return MATCHMESH_VERSION;
}

}  // namespace matchmesh::core
