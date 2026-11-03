// CRPC -- cross-process TCP deployment check.
//
// Every other example in this directory uses the in-process backend: no
// sockets, one process, no operating-system network stack involved. That makes
// them good API demonstrations and useless as deployment evidence -- "all the
// examples pass" has never meant "a client in another process can reach this
// server".
//
// This one does involve the stack. Run with no arguments it spawns *itself* as
// a child process, so the server and the client are genuinely two processes
// talking over loopback TCP, and it exits non-zero if the round trip fails:
//
//     crpc_example_deploy                  # self-check (what CTest runs)
//     crpc_example_deploy serve <port>     # listener only, prints READY
//     crpc_example_deploy call  <port>     # client only, real GETs
//
// The split modes exist so a shell driver (scripts/deploy_check.ps1 / .sh) can
// also point an unrelated HTTP client at the same listener -- curl speaking to
// it is the proof that the wire format is ordinary HTTP/1.1 rather than
// something only this library can parse.
//
// Exit code 0 = validated; non-zero = failed (stderr explains what).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/http/message.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#if defined(_WIN32)
// winsock2.h must precede windows.h, so the socket types and the handle
// accounting header are pulled in together here rather than after it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <crpc/io/iocp_backend.hpp>
#elif defined(CRPC_OS_LINUX)
#include <crpc/io/epoll_backend.hpp>
#endif

namespace {

constexpr unsigned long kPortFallback = 19555;

// How long `serve` keeps waiting for another request before exiting on its own.
// The self-check parent cannot close the child's stdin through a one-way pipe,
// so the child bounds its own lifetime instead. Long enough that a sequence of
// client requests never races it, short enough that `pclose` returns promptly.
constexpr int kServeIdleSeconds = 5;

crpc::status handle_health(void*, const crpc::http::request&,
                           const crpc::mvc::route_params&, const crpc::codec&,
                           const crpc::codec_registry&,
                           crpc::http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = "ok";
    return crpc::status{};
}

crpc::status handle_echo(void*, const crpc::http::request& req,
                         const crpc::mvc::route_params& params,
                         const crpc::codec&, const crpc::codec_registry&,
                         crpc::http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    const std::string* word = params.find("word");
    out.body = word != nullptr ? *word : std::string(req.path);
    return crpc::status{};
}

crpc::mvc::router make_router() {
    crpc::mvc::router router;

    crpc::mvc::route_entry health;
    health.method = crpc::http::method::get;
    health.path = "/health";
    health.name = "health";
    health.invoke = &handle_health;
    (void)router.add(std::move(health));

    crpc::mvc::route_entry echo;
    echo.method = crpc::http::method::get;
    echo.path = "/echo/:word";
    echo.name = "echo";
    echo.invoke = &handle_echo;
    (void)router.add(std::move(echo));

    (void)router.freeze();
    return router;
}

// A backend the caller owns for the lifetime of one mode. Windows uses IOCP;
// Linux uses epoll. On any other platform this example reports that there is no
// real network backend to validate rather than pretending to pass.
struct backend_holder {
#if defined(_WIN32)
    crpc::iocp_backend value;
    [[nodiscard]] bool ok() const noexcept { return value.running(); }
#elif defined(CRPC_OS_LINUX)
    std::unique_ptr<crpc::io::epoll_backend> value;
    backend_holder() {
        auto created = crpc::io::epoll_backend::create();
        if (created.ok()) {
            value = std::move(created).value();
        }
    }
    [[nodiscard]] bool ok() const noexcept { return value != nullptr; }
#else
    backend_holder() = default;
    [[nodiscard]] bool ok() const noexcept { return false; }
#endif
    [[nodiscard]] crpc::backend_factory& factory() {
#if defined(_WIN32)
        return value;
#else
        return *value;
#endif
    }
};

// --- resource accounting -----------------------------------------------------
//
// A soak run is only meaningful if it can see the resource it is supposed to
// leak. Handle count is the crispest signal available without a profiler: an
// fd/socket leak after N connections shows up as a handle count that tracks the
// request count. RSS is noisy (allocators cache), and a leak of a few hundred
// bytes per request disappears into it.
//
// Windows exposes GetProcessHandleCount; Linux exposes the same fact as the
// number of entries in /proc/self/fd.
std::size_t process_handle_count() {
#if defined(_WIN32)
    DWORD count = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &count) == 0) {
        return 0;
    }
    return static_cast<std::size_t>(count);
#else
    std::error_code ec;
    std::size_t count = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++count;
    }
    return ec ? 0 : count;
#endif
}

int serve(unsigned long port, int idle_seconds) {
    backend_holder backend;
    if (!backend.ok()) {
        std::cerr << "deploy: network backend did not start\n";
        return 1;
    }

    const crpc::mvc::router router = make_router();

    crpc::http::http_server::builder builder;
    builder.add_listener(
        crpc::endpoint::tcp("127.0.0.1", static_cast<crpc::u16>(port)),
        backend.factory());
    builder.router(router);
    builder.shards(2);

    std::unique_ptr<crpc::http::http_server> server = builder.build();
    if (!server) {
        std::cerr << "deploy: build server failed\n";
        return 1;
    }
    const crpc::status started = server->start();
    if (!started.ok()) {
        std::cerr << "deploy: start failed: " << started.error().message << "\n";
        return 1;
    }

    // READY is the handshake: the parent launches a client only after this, so
    // no client ever races the listener's bind.
    std::cout << "READY " << port << std::endl;

    // Stay up while requests keep arriving; exit once the wire has been quiet
    // for kServeIdleSeconds. The parent cannot close our stdin through a
    // one-way pipe, so we bound our own lifetime.
    auto last_requests = server->stats().requests_handled;
    auto idle_since = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now_requests = server->stats().requests_handled;
        if (now_requests != last_requests) {
            last_requests = now_requests;
            idle_since = std::chrono::steady_clock::now();
            continue;
        }
        if (std::chrono::steady_clock::now() - idle_since >
            std::chrono::seconds(idle_seconds)) {
            break;
        }
    }

    const crpc::http::http_statistics stats = server->stats();
    std::cout << "STATS accepted=" << stats.accepted_connections
              << " requests=" << stats.requests_handled
              << " responses=" << stats.responses_written
              << " bad=" << stats.bad_requests
              << " handles=" << process_handle_count() << std::endl;
    server->stop();
    return 0;
}

struct collector final : crpc::io::source {
    std::mutex mu;
    std::string data;
    std::atomic<bool> closed{false};

    std::size_t on_data(crpc::span<const std::byte> bytes) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }
    void on_eof() noexcept override { closed.store(true); }
    void on_error(std::error_code) noexcept override { closed.store(true); }

    std::string snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return data;
    }
};

struct noop_completion final : crpc::io::completion {
    std::atomic<bool> done{false};
    void complete(crpc::io::io_result) noexcept override { done.store(true); }
};

std::string http_get(unsigned long port, std::string_view path) {
    backend_holder client_backend;
    if (!client_backend.ok()) {
        return {};
    }
    auto conn = client_backend.factory().connect(
        crpc::endpoint::tcp("127.0.0.1", static_cast<crpc::u16>(port)),
        crpc::connect_options{});
    if (!conn) {
        return {};
    }

    collector sink;
    conn->bind_source(&sink);

    std::string request;
    request += "GET ";
    request += path;
    request += " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";

    noop_completion op;
    conn->sink().push(crpc::span<const std::byte>(
                          reinterpret_cast<const std::byte*>(request.data()),
                          request.size()),
                      &op);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink.closed.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    conn->bind_source(nullptr);
    conn->close();
    return sink.snapshot();
}

// --- response checking -------------------------------------------------------
//
// A substring test is not enough here, and this was not hypothetical: while
// writing this check the server was mutated to answer "not-ok" and the check
// still passed, because `find("ok") != npos` matches "not-ok". A deployment
// gate that accepts the wrong body only proves a port accepted a connection.
//
// So parse the status line and the body precisely: split on the blank line,
// require the status line to be exactly `HTTP/1.1 <code> ...`, and compare the
// body byte for byte.
struct http_reply {
    bool parsed = false;
    int status = 0;
    std::string body;
};

http_reply parse_reply(const std::string& wire) {
    http_reply reply;
    const std::size_t line_end = wire.find("\r\n");
    if (line_end == std::string::npos) {
        return reply;
    }
    const std::string status_line = wire.substr(0, line_end);
    const std::string prefix = "HTTP/1.1 ";
    if (status_line.rfind(prefix, 0) != 0 || status_line.size() < prefix.size() + 3) {
        return reply;
    }
    for (int i = 0; i < 3; ++i) {
        const char c = status_line[prefix.size() + static_cast<std::size_t>(i)];
        if (c < '0' || c > '9') {
            return reply;
        }
    }
    reply.status = (status_line[prefix.size()] - '0') * 100 +
                   (status_line[prefix.size() + 1] - '0') * 10 +
                   (status_line[prefix.size() + 2] - '0');
    // The code must end there: `HTTP/1.1 2000` is not a 200.
    const std::size_t after = prefix.size() + 3;
    if (after < status_line.size() && status_line[after] >= '0' &&
        status_line[after] <= '9') {
        return reply;
    }

    const std::size_t blank = wire.find("\r\n\r\n");
    if (blank == std::string::npos) {
        return reply;
    }
    reply.parsed = true;
    // The handlers under test answer with `Connection: close` and no
    // transfer-encoding, so the remainder is the body as-is.
    reply.body = wire.substr(blank + 4);
    return reply;
}

// Reports a failure with what actually arrived, then returns 1 or 0.
int check(const char* path, const std::string& wire, int want_status,
          std::string_view want_body) {
    const http_reply reply = parse_reply(wire);
    if (!reply.parsed) {
        std::cerr << "deploy: " << path << " produced no parseable reply ("
                  << wire.size() << " byte(s)): "
                  << wire.substr(0, std::min<std::size_t>(wire.size(), 200))
                  << "\n";
        return 1;
    }
    if (reply.status != want_status || reply.body != want_body) {
        std::cerr << "deploy: " << path << " expected " << want_status << " \""
                  << want_body << "\", received " << reply.status << " \""
                  << reply.body << "\"\n";
        return 1;
    }
    std::cout << "call: " << path << " -> " << reply.status << " "
              << reply.body << std::endl;
    return 0;
}

int call(unsigned long port) {
    int failures = 0;
    failures += check("/health", http_get(port, "/health"), 200, "ok");
    failures += check("/echo/deploy", http_get(port, "/echo/deploy"), 200,
                      "deploy");
    return failures == 0 ? 0 : 1;
}

#if defined(_WIN32)
#define CRPC_POPEN _popen
#define CRPC_PCLOSE _pclose
#else
#define CRPC_POPEN popen
#define CRPC_PCLOSE pclose
#endif

// Spawn this executable as `serve <port>` in a child process and talk to it.
// This is the mode CTest runs: it needs no shell script and no helper.
// Defined below; self_check runs a short soak against its own child server.
int soak(unsigned long port, int requests);

int self_check(const char* self, unsigned long port) {
    std::string command = "\"";
    command += self;
    command += "\" serve ";
    command += std::to_string(port);

    std::FILE* pipe = CRPC_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        std::cerr << "deploy: cannot spawn the server child process\n";
        return 1;
    }

    // Wait for the child's READY line. A child that dies immediately (port in
    // use, backend failure) shows up here as EOF.
    std::string ready;
    char buffer[256];
    bool saw_ready = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::fgets(buffer, sizeof(buffer), pipe) == nullptr) {
            break;
        }
        ready = buffer;
        if (ready.rfind("READY", 0) == 0) {
            saw_ready = true;
            break;
        }
    }
    if (!saw_ready) {
        std::cerr << "deploy: the server child never became ready\n";
        (void)CRPC_PCLOSE(pipe);
        return 1;
    }
    std::cout << "server: " << ready.substr(0, ready.find('\n')) << std::endl;

    int result = call(port);

    // A short soak against the same child, so the sustained-load path (handle
    // accounting, connection churn) is exercised by CTest and not only by a
    // manual run. 100 connections is enough to catch a per-connection leak and
    // still finishes in about a second.
    if (result == 0) {
        result = soak(port, 100) == 0 ? 0 : 1;
    }

    // Let the child drain and report its counters; it exits on its own idle
    // timer, so this blocks for at most kServeIdleSeconds.
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::cout << "server: " << buffer;
    }
    (void)CRPC_PCLOSE(pipe);
    return result;
}

// --- soak --------------------------------------------------------------------
//
// Sustained connection churn against a real listener, with handle accounting.
//
// The deployment question this answers is not "does one request work" -- the
// deployment check already covers that -- but "does the process survive ten
// thousand of them without accumulating handles". A socket or fd leak is
// invisible in a five-request smoke test and fatal in production, so this runs
// the same connect / send / receive / close cycle `requests` times from ONE
// client backend (the shape a real client has) and compares the handle count
// before and after.
//
// The assertion is deliberately about growth rather than an absolute number:
// absolute handle counts differ per platform and per OS build, but a leak is
// always "grew with the work".
int soak(unsigned long port, int requests) {
    backend_holder client_backend;
    if (!client_backend.ok()) {
        std::cerr << "deploy: network backend did not start\n";
        return 1;
    }
    auto& backend = client_backend.factory();

    // Warm up: the first few connections legitimately allocate one-time state
    // (thread pools, socket tables), and counting those as a leak would make
    // this check fail on every correct build.
    for (int i = 0; i < 25; ++i) {
        auto conn = backend.connect(
            crpc::endpoint::tcp("127.0.0.1", static_cast<crpc::u16>(port)),
            crpc::connect_options{});
        if (!conn) {
            std::cerr << "deploy: soak could not connect on warm-up\n";
            return 1;
        }
        conn->bind_source(nullptr);
        conn->close();
    }
    // Prove the instrument works before trusting it. A handle counter that
    // silently returns a constant makes every leak check below pass by
    // construction, which is the worst kind of green. Open a known number of
    // sockets without closing them and require the counter to see them.
    {
        const std::size_t probe_before = process_handle_count();
#if defined(_WIN32)
        std::vector<SOCKET> probe;
        for (int i = 0; i < 16; ++i) {
            SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr,
                                    0, WSA_FLAG_OVERLAPPED);
            if (s != INVALID_SOCKET) {
                probe.push_back(s);
            }
        }
        const std::size_t probe_during = process_handle_count();
        for (SOCKET s : probe) {
            ::closesocket(s);
        }
        if (probe_before == 0 || probe.size() < 16 ||
            probe_during < probe_before + probe.size() / 2) {
            std::cerr << "deploy: the handle counter cannot see a deliberate leak ("
                      << probe_before << " -> " << probe_during << " for "
                      << probe.size() << " leaked sockets), so the leak check "
                      << "below would be meaningless\n";
            return 1;
        }
#else
        std::vector<int> probe;
        for (int i = 0; i < 16; ++i) {
            const int fd = ::open("/dev/null", O_RDONLY);
            if (fd >= 0) {
                probe.push_back(fd);
            }
        }
        const std::size_t probe_during = process_handle_count();
        for (int fd : probe) {
            ::close(fd);
        }
        if (probe_before == 0 || probe.size() < 16 ||
            probe_during < probe_before + probe.size() / 2) {
            std::cerr << "deploy: the fd counter cannot see a deliberate leak ("
                      << probe_before << " -> " << probe_during << " for "
                      << probe.size() << " leaked fds), so the leak check "
                      << "below would be meaningless\n";
            return 1;
        }
#endif
        std::cout << "soak: handle counter verified (" << probe_before << " -> "
                  << probe_during << " for " << probe.size()
                  << " deliberate leaks)" << std::endl;
    }

    const std::size_t baseline = process_handle_count();
    if (baseline == 0) {
        std::cerr << "deploy: cannot read this process's handle count\n";
        return 1;
    }

    int failures = 0;
    std::vector<double> latencies_ms;
    latencies_ms.reserve(static_cast<std::size_t>(requests));
    double connect_ms_total = 0.0;
    double transfer_ms_total = 0.0;
    std::vector<double> connect_ms;
    // A failed connect has two very different meanings and the bare "failed at
    // request N" could not tell them apart. A refusal is immediate (the listener
    // answered RST); a connect that burns the full connect_timeout means the SYN
    // was never answered, i.e. the accept side stopped consuming. CI once failed
    // here with 24 consecutive connect failures that together used the whole
    // 60 s CTest timeout -- about 2.4 s each, which is far too slow for a
    // refusal and is what identified it as an accept-side stall rather than a
    // closed port. Printing the elapsed time is what makes that distinction
    // visible in the failure itself instead of having to infer it from a
    // timeout.
    double worst_connect_ms = 0.0;
    int connect_failures = 0;
    for (int i = 0; i < requests; ++i) {
        const auto began = std::chrono::steady_clock::now();
        auto conn = backend.connect(
            crpc::endpoint::tcp("127.0.0.1", static_cast<crpc::u16>(port)),
            crpc::connect_options{});
        if (!conn) {
            const double failed_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - began)
                    .count();
            if (failed_ms > worst_connect_ms) {
                worst_connect_ms = failed_ms;
            }
            ++connect_failures;
            // First few only: a sustained stall would otherwise bury every other
            // line of output under hundreds of identical messages.
            if (connect_failures <= 5) {
                std::cerr << "deploy: soak connect failed at request " << i
                          << " after " << failed_ms << " ms\n";
            }
            ++failures;
            continue;
        }
        const auto connected = std::chrono::steady_clock::now();
        {
            const double cms =
                std::chrono::duration<double, std::milli>(connected - began)
                    .count();
            connect_ms_total += cms;
            connect_ms.push_back(cms);
        }
        collector sink;
        conn->bind_source(&sink);

        const std::string request =
            "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        noop_completion op;
        conn->sink().push(
            crpc::span<const std::byte>(
                reinterpret_cast<const std::byte*>(request.data()),
                request.size()),
            &op);

        // Spin with yield() before falling back to sleeping. On Windows the
        // default timer resolution is ~15.6 ms, so `sleep_for(1ms)` actually
        // sleeps ~15.6 ms -- the first version of this loop did exactly that and
        // reported a p50 of 15.5 ms per request, which was the measurement, not
        // the server. Same trap test_framework.hpp documents for the unit
        // framework.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        unsigned spins = 0;
        while (std::chrono::steady_clock::now() < deadline && !sink.closed.load()) {
            if (spins < 20000) {
                ++spins;
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        conn->bind_source(nullptr);
        conn->close();

        const auto finished = std::chrono::steady_clock::now();
        transfer_ms_total +=
            std::chrono::duration<double, std::milli>(finished - connected).count();
        latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(finished - began).count());

        const http_reply reply = parse_reply(sink.snapshot());
        if (!reply.parsed || reply.status != 200 || reply.body != "ok") {
            if (failures < 5) {
                std::cerr << "deploy: soak request " << i << " answered "
                          << reply.status << " \"" << reply.body << "\"\n";
            }
            ++failures;
        }
    }

    // Give the backend's teardown a moment to run before measuring; a close
    // that has not finished yet is not a leak, but a close that never finishes
    // is, and it will still show up after this.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::size_t after = process_handle_count();

    std::cout << "soak: requests=" << requests << " failures=" << failures
              << " connect_failures=" << connect_failures
              << " worst_connect_ms=" << worst_connect_ms
              << " handles " << baseline << " -> " << after;
    if (!latencies_ms.empty()) {
        std::vector<double> sorted = latencies_ms;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t n = sorted.size();
        const auto at = [&](double q) {
            const std::size_t idx = static_cast<std::size_t>(
                q * static_cast<double>(n - 1));
            return sorted[idx];
        };
        std::cout << " per-request-ms p50=" << at(0.50) << " p99=" << at(0.99)
                  << " max=" << sorted.back();
        if (requests > 0) {
            std::cout << " [connect-avg="
                      << (connect_ms_total / static_cast<double>(requests))
                      << " transfer-avg="
                      << (transfer_ms_total / static_cast<double>(requests))
                      << "]";
        }
        if (!connect_ms.empty()) {
            std::vector<double> cs = connect_ms;
            std::sort(cs.begin(), cs.end());
            const std::size_t m = cs.size();
            const auto cat = [&](double q) {
                return cs[static_cast<std::size_t>(q * static_cast<double>(m - 1))];
            };
            std::cout << " [connect-ms p50=" << cat(0.50)
                      << " p90=" << cat(0.90) << " p99=" << cat(0.99)
                      << " max=" << cs.back() << "]";
        }
    }
    std::cout << std::endl;

    // A leak here is one handle per connection, so `requests` handles would be
    // the signature. 32 is slack for allocator and OS bookkeeping that is
    // bounded rather than proportional.
    constexpr std::size_t kHandleSlack = 32;
    const bool leaked = after > baseline && (after - baseline) > kHandleSlack;
    if (leaked) {
        std::cerr << "deploy: handle count grew by " << (after - baseline)
                  << " over " << requests
                  << " connections, which tracks the work rather than "
                     "amortising; that is a leak\n";
    }
    return (failures == 0 && !leaked) ? 0 : 1;
}

} // namespace
int main(int argc, char** argv) {
    const unsigned long fallback = kPortFallback;

    if (argc < 2) {
        // No arguments: CTest's shape. Pick a port that is unlikely to collide
        // with anything else in a parallel run, then spawn ourselves.
        const unsigned long port = fallback + 7;
        return self_check(argv[0], port);
    }

    const std::string mode = argv[1];
    const unsigned long parsed =
        argc >= 3 ? std::strtoul(argv[2], nullptr, 10) : 0;
    const unsigned long port = parsed != 0 ? parsed : fallback;

    if (mode == "serve") {
        const int idle =
            argc >= 4 ? std::atoi(argv[3]) : kServeIdleSeconds;
        return serve(port, idle > 0 ? idle : kServeIdleSeconds);
    }
    if (mode == "call") {
        return call(port);
    }
    if (mode == "soak") {
        // `soak <port> <requests>`: the server must already be running, so a
        // driver can measure both processes independently.
        const int requests = argc >= 4 ? std::atoi(argv[3]) : 2000;
        return soak(port, requests > 0 ? requests : 2000);
    }
    std::cerr << "usage: crpc_example_deploy [serve|call|soak] <port> [n]\n";
    return 2;
}
