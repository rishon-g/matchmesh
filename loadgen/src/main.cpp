#include <iostream>

#include "matchmesh/core/version.hpp"

int main() {
    std::cout << "matchmesh_loadgen v" << matchmesh::core::version() << " ready\n";
    return 0;
}
