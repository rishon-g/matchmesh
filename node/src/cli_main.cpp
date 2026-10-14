// matchmesh_cli — a tiny command-line client for poking at a cluster by hand.
//
//   matchmesh_cli --target node-a:50051 enqueue   <player> <rating> <region> <mode>
//   matchmesh_cli --target node-a:50051 get-lobby <player> <rating> <region> <mode>
//   matchmesh_cli --target node-a:50051 cancel    <player> <rating> <region> <mode>
//   matchmesh_cli --target node-a:50051 topology
//
// For get-lobby and cancel, the rating is only used to work out the player's
// skill bracket (and so their queue), exactly as the server does on enqueue.
//
// Prints which node the call went to and which node actually served it, so
// you can watch forwarding happen. Exits 0 on success, 1 on any error.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "matchmesh/core/types.hpp"
#include "matchmesh/node/matchmaking_service.hpp"
#include "matchmesh/v1/cluster.grpc.pb.h"
#include "matchmesh/v1/matchmaking.grpc.pb.h"

namespace {

using matchmesh::v1::MatchmakingService;

void print_usage() {
    std::cerr << "usage: matchmesh_cli --target <host:port> <command> <player> <rating> <region> "
                 "<mode>\n"
              << "       matchmesh_cli --target <host:port> topology\n"
              << "commands: enqueue, get-lobby, cancel, topology\n";
}

std::string served_by(const grpc::ClientContext& context) {
    using matchmesh::node::kServedByKey;
    const auto& metadata = context.GetServerInitialMetadata();
    const auto it = metadata.find(grpc::string_ref(kServedByKey.data(), kServedByKey.size()));
    return it == metadata.end() ? "?" : std::string(it->second.data(), it->second.size());
}

void prepare(grpc::ClientContext& context) {
    context.set_wait_for_ready(true);  // tolerate a node that's still starting
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
}

int report_error(const grpc::Status& status) {
    std::cerr << "error: " << status.error_message() << " (code " << status.error_code() << ")\n";
    return EXIT_FAILURE;
}

}  // namespace

// Prints the membership view as the target node sees it.
int show_topology(const std::string& target) {
    const auto stub = matchmesh::v1::ClusterService::NewStub(
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    prepare(context);
    matchmesh::v1::GetTopologyResponse response;
    if (const grpc::Status status =
            stub->GetTopology(&context, matchmesh::v1::GetTopologyRequest{}, &response);
        !status.ok()) {
        return report_error(status);
    }
    std::cout << "epoch " << response.epoch() << ", coordinator "
              << response.coordinator_node_id() << "  (asked " << target << ")\n";
    for (const auto& node : response.nodes()) {
        std::cout << "  " << node.node_id() << "  " << (node.alive() ? "member" : "DOWN  ")
                  << "  " << node.address() << "\n";
    }
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 3 && args[0] == "--target" && args[2] == "topology") {
        return show_topology(args[1]);
    }
    if (args.size() != 7 || args[0] != "--target") {
        print_usage();
        return EXIT_FAILURE;
    }
    const std::string& target = args[1];
    const std::string& command = args[2];
    const std::string& player = args[3];
    const std::string& region = args[5];
    const std::string& mode = args[6];
    int rating = 0;
    try {
        rating = std::stoi(args[4]);
    } catch (const std::exception&) {
        std::cerr << "rating must be a number\n";
        return EXIT_FAILURE;
    }

    const auto stub = MatchmakingService::NewStub(
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    prepare(context);

    // The queue this player belongs to, as the server would compute it.
    matchmesh::v1::QueueKey queue;
    queue.set_region(region);
    queue.set_game_mode(mode);
    queue.set_skill_bracket(matchmesh::core::skill_bracket_for(rating));
    const std::string queue_name = region + "/" + mode + "/" + std::to_string(queue.skill_bracket());

    if (command == "enqueue") {
        matchmesh::v1::EnqueueRequest request;
        request.mutable_player()->set_player_id(player);
        request.mutable_player()->set_rating(rating);
        request.set_region(region);
        request.set_game_mode(mode);
        matchmesh::v1::EnqueueResponse response;
        if (const grpc::Status status = stub->Enqueue(&context, request, &response); !status.ok()) {
            return report_error(status);
        }
        std::cout << "enqueued " << player << " into " << queue_name << "  (asked " << target
                  << ", served by " << served_by(context) << ")\n";
        return EXIT_SUCCESS;
    }

    if (command == "get-lobby") {
        matchmesh::v1::GetLobbyRequest request;
        request.set_player_id(player);
        *request.mutable_queue() = queue;
        matchmesh::v1::GetLobbyResponse response;
        if (const grpc::Status status = stub->GetLobby(&context, request, &response); !status.ok()) {
            return report_error(status);
        }
        std::cout << player << ": " << matchmesh::v1::PlayerStatus_Name(response.status());
        if (response.has_lobby()) {
            std::cout << " " << response.lobby().lobby_id() << " [";
            for (int i = 0; i < response.lobby().players_size(); ++i) {
                std::cout << (i ? ", " : "") << response.lobby().players(i).player_id();
            }
            std::cout << "]";
        }
        std::cout << "  (asked " << target << ", served by " << served_by(context) << ")\n";
        return EXIT_SUCCESS;
    }

    if (command == "cancel") {
        matchmesh::v1::CancelRequest request;
        request.set_player_id(player);
        *request.mutable_queue() = queue;
        matchmesh::v1::CancelResponse response;
        if (const grpc::Status status = stub->Cancel(&context, request, &response); !status.ok()) {
            return report_error(status);
        }
        std::cout << player << (response.cancelled() ? " cancelled" : " was not waiting")
                  << "  (asked " << target << ", served by " << served_by(context) << ")\n";
        return EXIT_SUCCESS;
    }

    print_usage();
    return EXIT_FAILURE;
}
