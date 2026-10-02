#include <iostream>

#include "matchmesh/core/version.hpp"

int main() {
    std::cout << "matchmesh_node v" << matchmesh::core::version() << " starting\n";
    return 0;
}
