// CRPC -- G19 step2: registryd, the registry service executable.
//
// This is a thin main: configuration, signal handling, start, stop. All the
// behaviour lives in `cluster::registry_service`.
//
//   registryd --state-dir <dir> --token <bearer-token> [--port <n>] [--admin <tok>]
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

#include <crpc/cluster/registry_service.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>

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
                 "[--port <n>] [--admin <token>] [--node <node-id>]\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    std::string state_dir;
    std::string token;
    std::string admin_token;
    std::string node_id = "node-A";
    crpc::u16 port = 0;

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

    crpc::cluster::registry_service_config config;
    config.state_dir = crpc::fs::path(state_dir);
    config.listen = crpc::endpoint::tcp("127.0.0.1", port);
    config.backend = backend.get();
    config.registry.admin_token = admin_token;

    crpc::cluster::acl_rule rule;
    rule.token_hash = hash;
    rule.principal_id = node_id;
    rule.node_id = node_id;
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

    const crpc::endpoint bound = service->bound_endpoint();
    std::printf("registryd: listening on %s:%u (state=%s)\n", bound.host().c_str(),
                static_cast<unsigned>(bound.port()), state_dir.c_str());
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
    service->stop();
    service.reset();
    backend.reset();
    return 0;
}
