// CRPC -- G19 step2: registry service cross-process tests (E1 / E4).
//
// The parent hosts the service (a real `registry_service` on a real TCP
// listener); the child is this same binary invoked with `--registry-client`,
// so no second executable is needed and the exit code is the report. This is
// the pattern tests/test_ipc_cross_process.cpp established, and it avoids the
// fragility of capturing a child's stdout: the child's own assertions are its
// exit code.
//
// The two Windows traps BRIEF section 0 names are handled:
//   * text mode: nothing here compares protocol bytes, and the child writes
//     nothing to stdout;
//   * inherited stdin at EOF: the child never reads stdin, and the parent gives
//     it a dedicated pipe anyway.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/registry_client.hpp>
#include <crpc/cluster/registry_service.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#endif

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace std::chrono_literals;

int run_client(crpc::u16 port, const std::string& token, const std::string& action);

namespace {

constexpr const char* kClientFlag = "--registry-client";



crpc::u64 fnv1a(const std::string& value) {
    crpc::u64 hash = 1469598103934665603ull;
    for (const char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

crpc::u16 next_port() {
    static std::atomic<unsigned> counter{0};
#if defined(_WIN32)
    static const unsigned base = 32000u + (::GetCurrentProcessId() % 20000u);
#else
    static const unsigned base = 32000u + (static_cast<unsigned>(::getpid()) % 20000u);
#endif
    const unsigned value = base + counter.fetch_add(19, std::memory_order_relaxed);
    return static_cast<crpc::u16>(value % 60000u + 1024u);
}

#if defined(_WIN32)

struct child_process {
    PROCESS_INFORMATION pi{};
    ~child_process() {
        if (pi.hProcess != nullptr) {
            ::CloseHandle(pi.hProcess);
        }
        if (pi.hThread != nullptr) {
            ::CloseHandle(pi.hThread);
        }
    }
};

#else
struct child_process {
    pid_t pid = -1;
};
#endif



// Spawns this binary as the registry client. `port` is the service's port,
// `token` the ACL token, `action` what the child should do.

bool spawn_client(crpc::u16 port, const std::string& token, const std::string& action,
                  child_process& out) {
#if defined(_WIN32)
    char exe[MAX_PATH] = {0};
    if (::GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) {
        return false;
    }
    std::string cmdline = std::string("\"") + exe + "\" " + kClientFlag +
                          " --port " + std::to_string(port) +
                          " --token " + token + " --action " + action;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    // A dedicated stdin pipe: the child does not read stdin, but an inherited
    // handle at EOF is an unnecessary hazard. Both ends must be valid pointers
    // -- CreatePipe writes the write end even when the caller discards it.
    HANDLE child_stdin = nullptr;
    HANDLE child_stdout_unused = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (::CreatePipe(&child_stdin, &child_stdout_unused, &sa, 0) == 0) {
        return false;
    }
    si.hStdInput = child_stdin;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    const bool ok = ::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, 0,
                                     nullptr, nullptr, &si, &out.pi) != 0;
    if (child_stdin != nullptr) {
        ::CloseHandle(child_stdin);
    }
    if (child_stdout_unused != nullptr) {
        ::CloseHandle(child_stdout_unused);
    }
    return ok;
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        const int code = run_client(port, token, action);
        ::_exit(code);
    }
    out.pid = pid;
    return true;
#endif
}

bool wait_client(const child_process& child, int& exit_code, int timeout_ms) {
#if defined(_WIN32)
    const DWORD waited = ::WaitForSingleObject(child.pi.hProcess, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(child.pi.hProcess, 99);
        exit_code = -1;
        return false;
    }
    DWORD code = 0;
    (void)::GetExitCodeProcess(child.pi.hProcess, &code);
    exit_code = static_cast<int>(code);
    return true;
#else
    int status = 0;
    const pid_t got = ::waitpid(child.pid, &status, 0);
    if (got != child.pid || !WIFEXITED(status)) {
        exit_code = -1;
        return false;
    }
    exit_code = WEXITSTATUS(status);
    return true;
#endif
}

} // namespace

// --- The child's work --------------------------------------------------------

namespace {

int run_client_impl(crpc::u16 port, const std::string& token,
                    const std::string& action) {
    auto backend = std::make_unique<crpc::iocp_backend>();
    auto connection = backend->connect(crpc::endpoint::tcp("127.0.0.1", port),
                                       crpc::connect_options{});
    if (!connection) {
        return 11;
    }
    crpc::rpc::client_options options;
    options.default_timeout = 2s;
    auto client =
        std::make_shared<crpc::rpc::client>(std::move(connection), options);
    cl::registry_client registry(client, cl::registry_client_options{token});
    registry.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    if (action == "register") {
        const auto written = registry.register_instance(instance, 1, 7);
        if (!written.ok()) {
            return 12;
        }
        const auto queried = registry.query("svc");
        if (!queried.ok() || queried.value().rows.size() != 1) {
            return 13;
        }
        return 0;
    }
    if (action == "register-then-check") {
        const auto written = registry.register_instance(instance, 1, 7);
        if (!written.ok()) {
            return 12;
        }
        return 0;
    }
    return 14;
}

} // namespace

namespace {

// Starts a service in this process and returns it with its bound endpoint.
struct hosted_service {
    std::unique_ptr<crpc::iocp_backend> backend;
    std::unique_ptr<cl::registry_service> service;
    crpc::endpoint bound_ep;
    fs::path state_dir;

    ~hosted_service() { stop(); }

    bool start(const std::string& dir, const std::string& token) {
        backend = std::make_unique<crpc::iocp_backend>();
        state_dir = fs::path(dir);
        cl::registry_service_config config;
        config.state_dir = state_dir;
        config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
        config.backend = backend.get();
        cl::acl_rule rule;
        rule.token_hash = fnv1a(token);
        rule.principal_id = "node-A";
        rule.node_id = "node-A";
        rule.read_prefixes.push_back("");
        rule.write_prefixes.push_back("");
        config.acl.push_back(rule);
        service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        if (!service->prepare(builder).ok()) {
            return false;
        }
        auto server = builder.build();
        if (!server || !server->start().ok()) {
            return false;
        }
        service->set_server(std::move(server));
        bound_ep = service->bound_endpoint();
        return bound_ep.port() != 0;
    }

    void stop() noexcept {
        service.reset();
        backend.reset();
    }
};

} // namespace

int run_client(crpc::u16 port, const std::string& token, const std::string& action) {
    return run_client_impl(port, token, action);
}

// --- E1 cross-process: a second process registers and queries ----------------

CRPC_TEST(cross_process_second_process_registers_and_queries) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_xproc_e1";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    hosted_service host;
    CRPC_REQUIRE(host.start(dir.str(), token));
    std::printf("host listening on port %u\n", static_cast<unsigned>(host.bound_ep.port()));
    std::fflush(stdout);

    child_process child;
    CRPC_REQUIRE(spawn_client(host.bound_ep.port(), token, "register", child));
    std::printf("child spawned\n");
    std::fflush(stdout);
    int exit_code = -1;
    CRPC_REQUIRE(wait_client(child, exit_code, 15000));
    std::printf("child exit=%d\n", exit_code);
    std::fflush(stdout);
    CRPC_CHECK_EQ(0, exit_code);

    // The parent must see what the child wrote, which is the point of a
    // registry: the catalog is not process-private.
    auto backend = std::make_unique<crpc::iocp_backend>();
    auto connection = backend->connect(host.bound_ep, crpc::connect_options{});
    CRPC_REQUIRE(connection != nullptr);
    crpc::rpc::client_options options;
    options.default_timeout = 2s;
    auto client =
        std::make_shared<crpc::rpc::client>(std::move(connection), options);
    cl::registry_client registry(client, cl::registry_client_options{token});
    registry.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
    const auto queried = registry.query("svc");
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());

    (void)fs::remove_all(dir);
}

// --- E1b: the catalog survives a service restart -----------------------------

CRPC_TEST(cross_process_catalog_survives_restart) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_xproc_e1b";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    {
        hosted_service host;
        CRPC_REQUIRE(host.start(dir.str(), token));
        child_process child;
        CRPC_REQUIRE(spawn_client(host.bound_ep.port(), token, "register-then-check", child));
        int exit_code = -1;
        CRPC_REQUIRE(wait_client(child, exit_code, 15000));
        CRPC_CHECK_EQ(0, exit_code);
    }

    // A second service over the same directory must restore the catalog.
    {
        hosted_service host;
        CRPC_REQUIRE(host.start(dir.str(), token));
        auto backend = std::make_unique<crpc::iocp_backend>();
        auto connection = backend->connect(host.bound_ep, crpc::connect_options{});
        CRPC_REQUIRE(connection != nullptr);
        crpc::rpc::client_options options;
        options.default_timeout = 2s;
        auto client =
            std::make_shared<crpc::rpc::client>(std::move(connection), options);
        cl::registry_client registry(client, cl::registry_client_options{token});
    registry.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
        const auto queried = registry.query("svc");
        CRPC_REQUIRE(queried.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());
    }

    (void)fs::remove_all(dir);
}

// --- E4: the authority unavailable fails closed ------------------------------

CRPC_TEST(cross_process_authority_unavailable_is_fail_closed) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_xproc_e4";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    hosted_service host;
    CRPC_REQUIRE(host.start(dir.str(), token));

    // A write that succeeds first: the fail-closed answer must be about the
    // missing authority, not about a request the service never accepted.
    {
        auto backend = std::make_unique<crpc::iocp_backend>();
        auto connection = backend->connect(host.bound_ep, crpc::connect_options{});
        CRPC_REQUIRE(connection != nullptr);
        crpc::rpc::client_options options;
        options.default_timeout = 2s;
        auto client =
            std::make_shared<crpc::rpc::client>(std::move(connection), options);
        cl::registry_client registry(client, cl::registry_client_options{token});
    registry.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
        cl::service_instance instance;
        instance.service = "svc";
        instance.node_id = "node-A";
        instance.address.host = "127.0.0.1";
        instance.address.port = 8080;
        const auto written = registry.register_instance(instance, 1, 7);
        CRPC_REQUIRE(written.ok());
    }

    host.stop();

    // After the stop, a connect must fail (the listener is gone), and a write
    // through a fresh client must be `unavailable`, never a false success.
    auto backend = std::make_unique<crpc::iocp_backend>();
    auto connection = backend->connect(host.bound_ep, crpc::connect_options{});
    CRPC_CHECK_FALSE(connection != nullptr);
    if (connection != nullptr) {
        crpc::rpc::client_options options;
        options.default_timeout = 1s;
        auto client =
            std::make_shared<crpc::rpc::client>(std::move(connection), options);
        cl::registry_client registry(client, cl::registry_client_options{token});
    registry.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
        cl::service_instance instance;
        instance.service = "svc";
        instance.node_id = "node-A";
        instance.address.host = "127.0.0.1";
        instance.address.port = 9090;
        const auto after = registry.register_instance(instance, 2, 8);
        // The write MUST fail: a timeout followed by a false success is the
        // exact hazard E4 exists to rule out.
        CRPC_CHECK_FALSE(after.ok());
        if (after.ok()) {
            return;
        }
        CRPC_CHECK(after.error().code == crpc::rpc_code::unavailable ||
                   after.error().code == crpc::rpc_code::connection_refused);
    }

    // And the failed write must have left no trace: a restarted service over
    // the same directory still has exactly the one row from before the stop,
    // with the original endpoint.
    {
        hosted_service restarted;
        CRPC_REQUIRE(restarted.start(dir.str(), token));
        auto restart_backend = std::make_unique<crpc::iocp_backend>();
        auto restart_connection =
            restart_backend->connect(restarted.bound_ep, crpc::connect_options{});
        CRPC_REQUIRE(restart_connection != nullptr);
        crpc::rpc::client_options options;
        options.default_timeout = 2s;
        auto client = std::make_shared<crpc::rpc::client>(std::move(restart_connection),
                                                           options);
        cl::registry_client registry(client, cl::registry_client_options{token});
    registry.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
        const auto queried = registry.query("svc");
        CRPC_REQUIRE(queried.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());
        if (!queried.value().rows.empty()) {
            CRPC_CHECK_EQ(static_cast<crpc::u16>(8080),
                          queried.value().rows.front().instance.address.port);
        }
    }

    (void)fs::remove_all(dir);
}

int main(int argc, char** argv) {
    CRPC_TEST_EARLY();
    if (argc >= 2 && std::strcmp(argv[1], kClientFlag) == 0) {
        crpc::u16 port = 0;
        std::string token;
        std::string action;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc) {
                port = static_cast<crpc::u16>(std::atoi(argv[++i]));
            } else if (arg == "--token" && i + 1 < argc) {
                token = argv[++i];
            } else if (arg == "--action" && i + 1 < argc) {
                action = argv[++i];
            }
        }
        return run_client(port, token, action);
    }
    return ::crpc_test::run_all(argc, argv);
}
