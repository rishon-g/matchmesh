// matchmesh_node — Phase 1 smoke-test build.
//
// Two modes:
//   matchmesh_node --id node-a --listen 0.0.0.0:50051
//       Start a gRPC server that answers ClusterService.Heartbeat.
//   matchmesh_node --id node-b --ping node-a:50051
//       Send one Heartbeat to a peer, print the reply, exit 0 on success.
//
// Only Heartbeat is implemented so far. Every other RPC returns gRPC's
// default UNIMPLEMENTED status until later phases fill them in.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "matchmesh/core/version.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"

namespace {

struct Options {
    std::string node_id = "node";
    std::string listen_address;  // server mode when set
    std::string ping_target;     // client mode when set
};

void print_usage() {
    std::cerr << "usage:\n"
              << "  matchmesh_node --id <node-id> --listen <host:port>\n"
              << "  matchmesh_node --id <node-id> --ping <host:port>\n";
}

// Returns false if the arguments don't make sense.
bool parse_args(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            return false;  // every flag takes a value
        }
        const std::string value = argv[++i];
        if (flag == "--id") {
            out.node_id = value;
        } else if (flag == "--listen") {
            out.listen_address = value;
        } else if (flag == "--ping") {
            out.ping_target = value;
        } else {
            return false;
        }
    }
    // Exactly one of the two modes.
    return out.listen_address.empty() != out.ping_target.empty();
}

// Server side: the generated ClusterService::Service base class declares one
// virtual method per RPC. We override only Heartbeat.
class ClusterServiceImpl final : public matchmesh::v1::ClusterService::Service {
public:
    explicit ClusterServiceImpl(std::string node_id) : node_id_(std::move(node_id)) {}

    grpc::Status Heartbeat(grpc::ServerContext* context,
                           const matchmesh::v1::HeartbeatRequest* request,
                           matchmesh::v1::HeartbeatResponse* response) override {
        std::cout << "heartbeat from " << request->node_id() << " (" << context->peer() << ")" << std::endl;
        response->set_node_id(node_id_);
        response->set_epoch(0);  // membership epochs arrive in Phase 5
        return grpc::Status::OK;
    }

private:
    std::string node_id_;
};

int run_server(const Options& options) {
    ClusterServiceImpl service(options.node_id);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(options.listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "failed to listen on " << options.listen_address << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "matchmesh_node v" << matchmesh::core::version() << " [" << options.node_id
              << "] listening on " << options.listen_address << std::endl;
    server->Wait();  // blocks until the process is stopped
    return EXIT_SUCCESS;
}

int run_ping(const Options& options) {
    const auto channel =
        grpc::CreateChannel(options.ping_target, grpc::InsecureChannelCredentials());
    const auto stub = matchmesh::v1::ClusterService::NewStub(channel);

    matchmesh::v1::HeartbeatRequest request;
    request.set_node_id(options.node_id);
    request.set_epoch(0);

    grpc::ClientContext context;
    // Give the peer up to 5 s to come up, instead of failing on the first
    // attempt if its container is still starting.
    context.set_wait_for_ready(true);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    matchmesh::v1::HeartbeatResponse response;
    const auto start = std::chrono::steady_clock::now();
    const grpc::Status status = stub->Heartbeat(&context, request, &response);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    if (!status.ok()) {
        std::cerr << "ping " << options.ping_target << " failed: " << status.error_message()
                  << " (code " << status.error_code() << ")\n";
        return EXIT_FAILURE;
    }
    std::cout << "pong from " << response.node_id() << " at " << options.ping_target << " in "
              << static_cast<double>(elapsed.count()) / 1000.0 << " ms\n";
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage();
        return EXIT_FAILURE;
    }
    return options.listen_address.empty() ? run_ping(options) : run_server(options);
}
