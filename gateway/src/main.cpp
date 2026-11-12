// matchmesh_gateway — the bridge between the browser dashboard and the
// cluster. Browsers can't call gRPC, so this polls every node's GetTopology,
// merges the answers into one JSON picture of the cluster, and pushes it to
// every connected browser over a WebSocket whenever it changes.
//
//   matchmesh_gateway --nodes node-a=node-a:50051,node-b=node-b:50051,...
//                     [--listen 0.0.0.0:8081] [--interval 500]
//
//   GET /ws             WebSocket: the current snapshot on connect, then each new one
//   GET /api/topology   the current snapshot, once (for scripts and debugging)
//   GET /healthz        "ok"
//
//   --nodes     every node, id=host:port (or MATCHMESH_PEERS, the same list the nodes use)
//   --listen    where to serve (or MATCHMESH_GATEWAY_LISTEN; default 0.0.0.0:8081)
//   --interval  milliseconds between polls (default 500)

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <crow.h>

#include "matchmesh/core/version.hpp"
#include "matchmesh/gateway/poller.hpp"
#include "matchmesh/node/router.hpp"

namespace {

struct Options {
    std::string nodes;
    std::string listen = "0.0.0.0:8081";
    int interval_ms = 500;
};

bool parse_args(int argc, char** argv, Options& out) {
    if (const char* peers = std::getenv("MATCHMESH_PEERS"); peers != nullptr) {
        out.nodes = peers;
    }
    if (const char* listen = std::getenv("MATCHMESH_GATEWAY_LISTEN"); listen != nullptr) {
        out.listen = listen;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string value = argv[i + 1];
        if (flag == "--nodes") {
            out.nodes = value;
        } else if (flag == "--listen") {
            out.listen = value;
        } else if (flag == "--interval") {
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), out.interval_ms);
            if (error != std::errc{} || end != value.data() + value.size() ||
                out.interval_ms <= 0) {
                return false;
            }
        } else {
            return false;
        }
    }
    return argc % 2 == 1 && !out.nodes.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        std::cerr << "usage: matchmesh_gateway --nodes id=host:port,... [--listen host:port] "
                     "[--interval ms]\n";
        return EXIT_FAILURE;
    }
    std::map<std::string, std::string> nodes;
    try {
        nodes = matchmesh::node::parse_peers(options.nodes);
    } catch (const std::invalid_argument& error) {
        std::cerr << "bad node list: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    const auto colon = options.listen.rfind(':');
    const char* const listen_end = options.listen.data() + options.listen.size();
    std::uint16_t port = 0;
    // Also reject trailing characters, so "host:80x" isn't read as port 80.
    if (colon == std::string::npos ||
        std::from_chars(options.listen.data() + colon + 1, listen_end, port) !=
            std::from_chars_result{listen_end, std::errc{}}) {
        std::cerr << "--listen must be host:port\n";
        return EXIT_FAILURE;
    }

    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);  // no line per request

    // Open dashboard connections. Crow calls these handlers on its own
    // threads and the poller pushes from its thread, so one mutex covers both.
    std::mutex clients_mutex;
    std::unordered_set<crow::websocket::connection*> clients;

    matchmesh::gateway::TopologyPoller poller(
        nodes,
        [&](const std::string& snapshot) {
            const std::scoped_lock lock(clients_mutex);
            for (crow::websocket::connection* client : clients) {
                client->send_text(snapshot);  // Crow queues it on the connection's own thread
            }
        },
        std::chrono::milliseconds(options.interval_ms));

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&](crow::websocket::connection& client) {
            const std::scoped_lock lock(clients_mutex);
            clients.insert(&client);
            client.send_text(poller.latest());  // don't make a new viewer wait
        })
        .onclose([&](crow::websocket::connection& client, const std::string&, std::uint16_t) {
            const std::scoped_lock lock(clients_mutex);
            clients.erase(&client);
        })
        .onerror([&](crow::websocket::connection& client, const std::string&) {
            const std::scoped_lock lock(clients_mutex);
            clients.erase(&client);
        })
        .onmessage([](crow::websocket::connection&, const std::string&, bool) {
            // The dashboard only listens.
        });

    CROW_ROUTE(app, "/api/topology")([&] {
        crow::response response(poller.latest());
        response.set_header("Content-Type", "application/json");
        return response;
    });
    CROW_ROUTE(app, "/healthz")([] { return "ok"; });

    std::cout << "matchmesh_gateway v" << matchmesh::core::version() << " polling "
              << nodes.size() << " node(s) every " << options.interval_ms << " ms, serving on "
              << options.listen << std::endl;
    poller.start();
    // Crow handles Ctrl+C / docker stop itself and returns from run().
    app.bindaddr(options.listen.substr(0, colon)).port(port).multithreaded().run();
    poller.stop();
    return EXIT_SUCCESS;
}
