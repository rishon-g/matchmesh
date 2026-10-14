// matchmesh_node — one MatchMesh node.
//
// Two modes:
//   matchmesh_node --id node-a --listen 0.0.0.0:50051 [--workers 4]
//                  [--peers node-a=node-a:50051,node-b=node-b:50051,...]
//       Serve MatchmakingService (Enqueue, Cancel, GetLobby) and
//       ClusterService.Heartbeat, with a pool of matchmaking worker threads.
//       With --peers, queues are spread over all listed nodes and requests
//       for another node's queue are forwarded to it.
//   matchmesh_node --id node-b --ping node-a:50051
//       Send one Heartbeat to a peer, print the reply, exit 0 on success.
//
// Every flag can also come from an environment variable, which is how Docker
// Compose configures each node. A flag wins over its variable.
//   --id       MATCHMESH_NODE_ID
//   --listen   MATCHMESH_LISTEN
//   --peers    MATCHMESH_PEERS
//   --workers  MATCHMESH_WORKERS

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <pthread.h>
#include <grpcpp/grpcpp.h>

#include "matchmesh/core/engine.hpp"
#include "matchmesh/core/version.hpp"
#include "matchmesh/node/cluster_service.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/node/router.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"

namespace {

struct Options {
    std::string node_id = "node";
    std::string listen_address;  // server mode when set
    std::string ping_target;     // client mode when set
    std::string peers;           // "id=host:port,..." (server mode)
    std::size_t workers = 4;     // matchmaking worker threads (server mode)
};

void print_usage() {
    std::cerr << "usage:\n"
              << "  matchmesh_node --id <node-id> --listen <host:port> [--workers <n>]\n"
              << "                 [--peers <id=host:port,...>]\n"
              << "  matchmesh_node --id <node-id> --ping <host:port>\n"
              << "flags may instead come from MATCHMESH_NODE_ID, MATCHMESH_LISTEN,\n"
              << "MATCHMESH_PEERS and MATCHMESH_WORKERS\n";
}

std::optional<std::string> env(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    return std::nullopt;
}

bool parse_workers(const std::string& value, std::size_t& out) {
    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed == 0 || parsed > 256) {
            return false;
        }
        out = parsed;
        return true;
    } catch (const std::exception&) {
        return false;  // not a number
    }
}

// Returns false if the arguments don't make sense.
bool parse_args(int argc, char** argv, Options& out) {
    // Environment first, so flags given on the command line override it.
    if (auto id = env("MATCHMESH_NODE_ID")) {
        out.node_id = *id;
    }
    if (auto listen = env("MATCHMESH_LISTEN")) {
        out.listen_address = *listen;
    }
    if (auto peers = env("MATCHMESH_PEERS")) {
        out.peers = *peers;
    }
    if (auto workers = env("MATCHMESH_WORKERS"); workers && !parse_workers(*workers, out.workers)) {
        return false;
    }

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
            out.listen_address.clear();  // an explicit --ping means ping mode
        } else if (flag == "--peers") {
            out.peers = value;
        } else if (flag == "--workers") {
            if (!parse_workers(value, out.workers)) {
                return false;
            }
        } else {
            return false;
        }
    }
    // Exactly one of the two modes.
    return out.listen_address.empty() != out.ping_target.empty();
}

int run_server(const Options& options) {
    // Route Ctrl+C (SIGINT) and `docker stop` (SIGTERM) to one dedicated
    // thread instead of interrupting whichever thread happens to be running.
    // Blocking them here, before any other thread exists, means every thread
    // created later (gRPC's and our workers) inherits the block.
    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

    std::map<std::string, std::string> peers;
    try {
        peers = matchmesh::node::parse_peers(options.peers);
    } catch (const std::invalid_argument& error) {
        std::cerr << "bad peer list: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    // Declaration order matters: destroyed in reverse, so the server stops
    // before the services it calls, and those before what they use.
    const matchmesh::node::Router router(options.node_id, peers);
    matchmesh::core::MatchmakingEngine engine(
        options.node_id, matchmesh::core::EngineConfig{.worker_count = options.workers});
    matchmesh::node::ClusterServiceImpl cluster_service(options.node_id);
    matchmesh::node::MatchmakingServiceImpl matchmaking_service(engine, router);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(options.listen_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&cluster_service);
    builder.RegisterService(&matchmaking_service);
    const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "failed to listen on " << options.listen_address << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "matchmesh_node v" << matchmesh::core::version() << " [" << options.node_id
              << "] listening on " << options.listen_address << " with " << options.workers
              << " matchmaking workers" << std::endl;
    std::cout << "cluster: " << router.ring().node_count() << " node(s), " << options.node_id
              << " (self)";
    for (const auto& [id, address] : router.peers()) {
        std::cout << ", " << id << " @ " << address;
    }
    std::cout << std::endl;

    // Waits for a shutdown signal, then lets in-flight calls finish (up to
    // 2 s) and stops the server, which makes server->Wait() below return.
    std::jthread signal_waiter([&server, shutdown_signals] {
        int signal_number = 0;
        sigwait(&shutdown_signals, &signal_number);
        std::cout << "received signal " << signal_number << ", shutting down" << std::endl;
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    });

    server->Wait();
    std::cout << "stopped after forming " << engine.lobbies_formed() << " lobbies" << std::endl;
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
