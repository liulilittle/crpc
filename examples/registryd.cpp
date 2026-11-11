// CRPC -- G19 step2: registryd, the registry service executable.
//
// This is a thin main: configuration, signal handling, start, stop. All the
// behaviour lives in `cluster::registry_service`.
//
//   registryd --state-dir <dir> --token <bearer-token>
//             [--port <n>] [--http-port <n>] [--admin <tok>] [--node <id>]
//
// Two channels serve the same service:
//
//   * the CRPC binary channel on `--port` (the native protocol);
//   * the Consul-compatible HTTP facade on `--http-port`, so an existing Consul
//     client works without a shim. Both reach the same handlers through
//     `registry_service::dispatch`, so the ACL check, the anchor water-mark and
//     the log append cannot diverge between them.
//
// The ACL is a bearer-token authorisation layer, not an authentication layer
// (see include/crpc/cluster/registry_acl.hpp for the full trust boundary). The
// token travels in the DTO body; there is no TLS in the RPC layer.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <crpc/cluster/consul_http.hpp>
#include <crpc/cluster/kv_store.hpp>
#include <crpc/cluster/raft_node.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/cluster/registry_service.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/mvc/router.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

crpc::cluster::registry_service* g_service = nullptr;
std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true, std::memory_order_release);
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --state-dir <dir> --token <bearer-token> "
                 "[--port <n>] [--http-port <n>] [--admin <token>] "
                 "[--node <node-id>]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string state_dir;
    std::string token;
    std::string admin_token;
    std::string node_id = "node-A";
    crpc::u16 port = 0;
    crpc::u16 http_port = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (arg == "--state-dir") {
            if (!next(state_dir)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg == "--token") {
            if (!next(token)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg == "--admin") {
            if (!next(admin_token)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg == "--node") {
            if (!next(node_id)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (arg == "--port") {
            std::string value;
            if (!next(value)) {
                print_usage(argv[0]);
                return 2;
            }
            port = static_cast<crpc::u16>(std::atoi(value.c_str()));
        } else if (arg == "--http-port") {
            std::string value;
            if (!next(value)) {
                print_usage(argv[0]);
                return 2;
            }
            http_port = static_cast<crpc::u16>(std::atoi(value.c_str()));
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 2;
        }
    }

    if (state_dir.empty() || token.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    // The token hash is the same FNV-1a the registry uses for principal_hash,
    // so the ACL table and the registry agree on the identity function.
    crpc::u64 hash = 1469598103934665603ull;
    for (const char c : token) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }

    auto backend = std::make_unique<crpc::iocp_backend>();

    // The KV data plane. It must outlive the service, which borrows it.
    auto store = std::make_unique<crpc::cluster::kv_store>();

    // A single-node raft node backs the data plane. Without it a KV write is
    // applied to the in-memory store and NEVER logged, so a restart comes up
    // empty: the "authority" would be memory. One node is its own quorum, so the
    // commit path is the same code a real cluster uses, and the log is a file.
    const crpc::fs::path state_path = crpc::fs::path(state_dir);
    // The raft log is opened before `registry_service::prepare`, which is what
    // normally creates the state directory, so the directory must exist first.
    const crpc::status dir_created = crpc::fs::create_directories(state_path);
    if (!dir_created.ok()) {
        std::fprintf(stderr, "registryd: state dir create failed: %s\n",
                     dir_created.message().c_str());
        return 1;
    }
    auto opened = crpc::cluster::file_registry_log::open(
        (state_path / "raft.log").str());
    if (!opened.ok()) {
        std::fprintf(stderr, "registryd: raft log open failed: %s\n",
                     opened.error().message.c_str());
        return 1;
    }
    auto raft_log = std::move(opened).value();

    crpc::cluster::raft_config raft_config;
    raft_config.node_id = node_id;
    // No peers: a single-node cluster. It elects itself and commits alone.
    raft_config.election_timeout_min = std::chrono::milliseconds(1000);
    raft_config.election_timeout_max = std::chrono::milliseconds(2000);
    raft_config.heartbeat_interval = std::chrono::milliseconds(300);
    auto raft = std::make_unique<crpc::cluster::raft_node>(
        raft_config, *raft_log, state_path,
        crpc::cluster::system_monotonic_clock());
    const crpc::status raft_started = raft->start();
    if (!raft_started.ok()) {
        std::fprintf(stderr, "registryd: raft start failed: %s\n",
                     raft_started.message().c_str());
        return 1;
    }

    crpc::cluster::registry_service_config config;
    config.state_dir = state_path;
    config.listen = crpc::endpoint::tcp("127.0.0.1", port);
    config.backend = backend.get();
    config.kv = store.get();
    config.raft = raft.get();
    config.registry.admin_token = admin_token;

    crpc::cluster::acl_rule rule;
    rule.token_hash = hash;
    rule.principal_id = node_id;
    // The node binding is deliberately left EMPTY: an empty `node_id` means
    // "any node". The KV namespace is global and its handlers pass an empty
    // node id to `check_acl`, so a rule bound to one node would deny every KV
    // write with 403. Registration still enforces `context.node_id ==
    // instance.node_id` through the registry's own authority check, which uses
    // `principal_id` (set above).
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config.acl.push_back(rule);

    auto service =
        std::make_unique<crpc::cluster::registry_service>(std::move(config));

    crpc::server::builder builder;
    const crpc::status prepared = service->prepare(builder);
    if (!prepared.ok()) {
        std::fprintf(stderr, "registryd: prepare failed: %s\n",
                     prepared.message().c_str());
        return 1;
    }
    auto server = builder.build();
    if (!server) {
        std::fprintf(stderr, "registryd: server build failed\n");
        return 1;
    }
    const crpc::status started = server->start();
    if (!started.ok()) {
        std::fprintf(stderr, "registryd: start failed: %s\n",
                     started.message().c_str());
        return 1;
    }
    service->set_server(std::move(server));
    g_service = service.get();

    // The raft node needs its timer driven: an election happens only when
    // `tick()` observes the deadline pass. A single node elects itself on the
    // first deadline, so this thread settles within one election timeout and
    // then only produces (empty) heartbeats.
    std::atomic<bool> raft_stop{false};
    std::thread raft_thread([&raft, &raft_stop] {
        while (!raft_stop.load(std::memory_order_acquire)) {
            raft->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    // Wait for leadership before serving. A write that arrived first would be
    // refused with `unavailable` ("only the leader can propose"), and a client
    // that retried would see a spurious failure on every cold start.
    for (int i = 0; i < 500 && raft->role() != crpc::cluster::raft_role::leader;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (raft->role() != crpc::cluster::raft_role::leader) {
        std::fprintf(stderr, "registryd: raft did not become leader\n");
        raft_stop.store(true, std::memory_order_release);
        raft_thread.join();
        return 1;
    }

    const crpc::endpoint bound = service->bound_endpoint();
    std::printf("registryd: listening on %s:%u (state=%s)\n", bound.host().c_str(),
                static_cast<unsigned>(bound.port()), state_dir.c_str());

    // The Consul-compatible channel. It is optional: a deployment that only
    // speaks the binary protocol pays for no HTTP listener. The facade and its
    // router must outlive the server, which borrows both.
    std::unique_ptr<crpc::http::http_server> http_server;
    std::unique_ptr<crpc::cluster::consul_http> facade;
    std::unique_ptr<crpc::mvc::router> routes;
    // A second backend for the HTTP listener. The CRPC server already owns
    // listeners on `backend`, and a second listener on the same factory fails
    // to bind ("listener could not bind endpoint") even on a distinct port, so
    // the two channels do not share one.
    std::unique_ptr<crpc::iocp_backend> http_backend;
    if (http_port != 0) {
        http_backend = std::make_unique<crpc::iocp_backend>();
        facade = std::make_unique<crpc::cluster::consul_http>(*service);
        routes = std::make_unique<crpc::mvc::router>();
        const crpc::status added = facade->add_routes(*routes);
        if (!added.ok()) {
            std::fprintf(stderr, "registryd: HTTP route setup failed: %s\n",
                         added.message().c_str());
            return 1;
        }
        // The server refuses to start on an unfrozen router.
        const crpc::status frozen = routes->freeze();
        if (!frozen.ok()) {
            std::fprintf(stderr, "registryd: HTTP router freeze failed: %s\n",
                         frozen.message().c_str());
            return 1;
        }
        crpc::http::http_server::builder http_builder;
        http_builder.add_listener(crpc::endpoint::tcp("127.0.0.1", http_port),
                                  *http_backend);
        http_builder.router(*routes);
        http_server = http_builder.build();
        if (!http_server) {
            std::fprintf(stderr, "registryd: HTTP server build failed\n");
            return 1;
        }
        const crpc::status http_started = http_server->start();
        if (!http_started.ok()) {
            std::fprintf(stderr, "registryd: HTTP start failed: %s\n",
                         http_started.message().c_str());
            return 1;
        }
        std::printf("registryd: HTTP (Consul API) on 127.0.0.1:%u\n",
                    static_cast<unsigned>(http_port));
    }
    std::fflush(stdout);

#if defined(_WIN32)
    (void)::SetConsoleCtrlHandler(nullptr, TRUE);
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    while (!g_stop.load(std::memory_order_acquire)) {
#if defined(_WIN32)
        ::Sleep(200);
#else
        ::usleep(200000);
#endif
    }

    std::printf("registryd: stopping\n");
    std::fflush(stdout);
    g_service = nullptr;
    // Stop the raft driver before tearing down the node it calls into.
    raft_stop.store(true, std::memory_order_release);
    raft_thread.join();
    if (http_server) {
        http_server->stop();
    }
    http_server.reset();
    facade.reset();
    routes.reset();
    http_backend.reset();
    service->stop();
    service.reset();
    raft.reset();
    raft_log.reset();
    store.reset();
    backend.reset();
    return 0;
}
