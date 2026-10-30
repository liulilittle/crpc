// CRPC -- IOCP transport backend tests.
//
// On a non-Windows target the whole file collapses to an empty main that exits
// zero, so the suite can include it unconditionally.  On Windows the tests
// exercise the contract the way the in-process tests do: round trips, credit
// backpressure, terminal notifications, close racing an in-flight write, and
// scale (hundreds of connections, ten thousand messages).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <crpc/io.hpp>

#if defined(_WIN32)

#include <crpc/io/iocp_backend.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace std::chrono;

// Candidate ports must stay clear of the Windows dynamic/ephemeral range
// (49152..65535): this process's own client connections and every other
// parallel ctest process allocate ephemeral ports there, so a bind inside
// that range collides under `ctest -j8`. The 20000..29999 band is below the
// ephemeral floor; the PID keeps parallel processes apart and the counter
// separates repeated calls in one process.
std::uint16_t next_port() {
    static std::mutex m;
    static unsigned counter = 0;
    std::lock_guard<std::mutex> lk(m);
    const unsigned base = 20000u + (::GetCurrentProcessId() % 5000u);
    const unsigned value = base + counter;
    ++counter;
    return static_cast<std::uint16_t>(20000u + value % 10000u);
}

// Binds the first free candidate port, retrying up to 16 times. Short-lived
// test processes recycle pids, and a previous incarnation's TIME_WAIT
// connections block the exclusive (SO_EXCLUSIVEADDRUSE) rebind of the same
// port, so a single pick fails roughly once per several thousand runs and
// turned long test loops flaky for reasons unrelated to what they exercise.
// `bound_port` receives the port that actually bound; callers must connect to
// it, not to the first pick.
std::unique_ptr<listener> listen_free(iocp_backend& be,
                                      std::uint16_t& bound_port) noexcept {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint16_t port = next_port();
        auto l = be.listen(endpoint::tcp("127.0.0.1", port), listen_options{});
        if (l != nullptr) {
            bound_port = port;
            return l;
        }
    }
    std::fprintf(stderr, "test_io_iocp: all 16 candidate ports failed to bind\n");
    std::fflush(stderr);
    return nullptr;
}

// Delegates to the harness helper, which spins before sleeping. A local
// sleep(1ms) loop is ~15.6ms per iteration on Windows and turned the 10k-message
// case into a 156-second "hang".
template <class Pred>
bool wait_for(Pred pred, int timeout_ms) {
    return ::crpc_test::wait_for(pred, timeout_ms);
}

span<const std::byte> bytes_of(const std::string& s) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()),
                                 s.size());
}

span<const std::byte> bytes_of(const std::vector<std::byte>& v) {
    return span<const std::byte>(v.data(), v.size());
}

struct recording_source final : io::source {
    std::mutex m;
    std::vector<std::byte> received;
    std::atomic<std::size_t> credit{static_cast<std::size_t>(-1)};

    std::atomic<int> data_calls{0};
    std::atomic<int> eof_calls{0};
    std::atomic<int> error_calls{0};
    std::atomic<int> last_error{0};

    std::size_t on_data(span<const std::byte> data) noexcept override {
        const std::size_t limit = credit.load();
        std::size_t take = 0;
        {
            std::lock_guard<std::mutex> lk(m);
            take = limit < data.size() ? limit : data.size();
            received.insert(received.end(), data.begin(),
                            data.begin() + static_cast<std::ptrdiff_t>(take));
        }
        data_calls.fetch_add(1);
        return take;
    }

    void on_eof() noexcept override { eof_calls.fetch_add(1); }

    void on_error(std::error_code ec) noexcept override {
        last_error.store(ec.value());
        error_calls.fetch_add(1);
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lk(m);
        return received.size();
    }
};

struct counting_completion final : io::completion {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> ok{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> broken{false};

    void complete(io::io_result result) noexcept override {
        bytes.store(result.bytes);
        ok.store(!result.ec);
        canceled.store(result.ec == std::errc::operation_canceled);
        broken.store(result.ec == std::errc::broken_pipe);
        calls.fetch_add(1);
    }
};

struct push_counter final : io::completion {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<int> errors{0};

    void complete(io::io_result result) noexcept override {
        if (result.ec) {
            errors.fetch_add(1);
        } else {
            bytes.fetch_add(result.bytes);
        }
        calls.fetch_add(1);
    }
};

// An echoing server source.  It never has more than one write outstanding: a
// received chunk is queued and flushed only when the previous write completes,
// so the test does not rely on the framework violating I5.
struct echo_source final : io::source {
    connection* conn = nullptr;
    std::mutex m;
    std::vector<std::byte> pending;
    std::vector<std::byte> inflight;
    bool busy = false;

    struct done_completion final : io::completion {
        echo_source* owner = nullptr;
        void complete(io::io_result) noexcept override { owner->on_done(); }
    } done_;

    echo_source() { done_.owner = this; }

    std::size_t on_data(span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lk(m);
            pending.insert(pending.end(), data.begin(), data.end());
        }
        try_flush();
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    void try_flush() {
        span<const std::byte> out;
        {
            std::lock_guard<std::mutex> lk(m);
            if (busy || pending.empty() || conn == nullptr) {
                return;
            }
            busy = true;
            inflight.swap(pending);
            out = bytes_of(inflight);
        }
        conn->sink().push(out, &done_);
    }

    void on_done() noexcept {
        {
            std::lock_guard<std::mutex> lk(m);
            busy = false;
            inflight.clear();
        }
        try_flush();
    }
};

struct conn_bundle {
    std::unique_ptr<connection> conn;
    echo_source src;
};

struct conn_bundle_plain {
    std::unique_ptr<connection> conn;
    recording_source src;
};

struct echo_server final : accept_sink {
    std::mutex m;
    std::vector<std::unique_ptr<conn_bundle>> bundles;
    std::atomic<int> accepts{0};
    bool bind_source = true;

    void on_accept(std::unique_ptr<connection> c) noexcept override {
        std::unique_ptr<conn_bundle> b(new (std::nothrow) conn_bundle());
        if (!b) {
            return;
        }
        b->conn = std::move(c);
        if (bind_source) {
            b->src.conn = b->conn.get();
            b->conn->bind_source(&b->src);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            bundles.push_back(std::move(b));
        }
        accepts.fetch_add(1);
    }

    connection* first_conn() {
        std::lock_guard<std::mutex> lk(m);
        return bundles.empty() ? nullptr : bundles.front()->conn.get();
    }
};

struct plain_server final : accept_sink {
    std::mutex m;
    std::vector<std::unique_ptr<conn_bundle_plain>> bundles;
    std::atomic<int> accepts{0};
    bool bind_source = true;

    void on_accept(std::unique_ptr<connection> c) noexcept override {
        std::unique_ptr<conn_bundle_plain> b(new (std::nothrow) conn_bundle_plain());
        if (!b) {
            return;
        }
        b->conn = std::move(c);
        if (bind_source) {
            b->conn->bind_source(&b->src);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            bundles.push_back(std::move(b));
        }
        accepts.fetch_add(1);
    }

    recording_source* first_source() {
        std::lock_guard<std::mutex> lk(m);
        return bundles.empty() ? nullptr : &bundles.front()->src;
    }
};

CRPC_TEST(iocp_listen_connect_echo) {
    iocp_backend server;
    iocp_backend client;
    CRPC_CHECK(server.running());
    CRPC_CHECK(client.running());

    echo_server ss;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ss);

    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ss.accepts.load() == 1; }, 5000));

    recording_source cs;
    conn->bind_source(&cs);

    const std::string msg = "hello-iocp";
    counting_completion done;
    conn->sink().push(bytes_of(msg), &done);
    CRPC_CHECK(wait_for([&] { return done.calls.load() == 1; }, 5000));
    CRPC_CHECK(done.ok.load());
    CRPC_CHECK(wait_for([&] { return cs.size() == msg.size(); }, 5000));
    CRPC_CHECK_EQ(msg.size(), cs.size());
    l->stop();
}

CRPC_TEST(iocp_large_transfer_small_queue) {
    iocp_options opt;
    opt.recv_buffer_bytes = 64 * 1024;
    opt.send_queue_bytes = 64 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);

    echo_server ss;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ss);

    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ss.accepts.load() == 1; }, 5000));

    recording_source cs;
    conn->bind_source(&cs);

    const std::size_t total = 1024 * 1024;
    const std::size_t chunk = 64 * 1024;
    std::vector<std::byte> buf(chunk);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::byte>('A' + (i % 26));
    }

    push_counter counter;
    std::size_t sent = 0;
    int pushes = 0;
    while (sent < total) {
        conn->sink().push(bytes_of(buf), &counter);
        ++pushes;
        const int expected = pushes;
        CRPC_CHECK(wait_for([&] { return counter.calls.load() >= expected; }, 5000));
        sent += chunk;
    }
    CRPC_CHECK(wait_for([&] { return cs.size() >= total; }, 10000));
    CRPC_CHECK_EQ(total, cs.size());
    CRPC_CHECK_EQ(0, counter.errors.load());
    l->stop();
}

CRPC_TEST(iocp_reader_pause_resume) {
    iocp_backend server;
    iocp_backend client;

    plain_server ps;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ps);

    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ps.accepts.load() == 1; }, 5000));

    recording_source cs;
    cs.credit = 0;
    conn->bind_source(&cs);

    connection* server_conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(ps.m);
        if (!ps.bundles.empty()) {
            server_conn = ps.bundles.front()->conn.get();
        }
    }
    CRPC_CHECK(server_conn != nullptr);
    if (!server_conn) {
        return;
    }

    const std::string payload(200, 'x');
    counting_completion done;
    server_conn->sink().push(bytes_of(payload), &done);
    CRPC_CHECK(wait_for([&] { return done.calls.load() == 1; }, 5000));
    CRPC_CHECK(wait_for([&] { return cs.data_calls.load() >= 1; }, 5000));
    CRPC_CHECK_EQ(0u, cs.size());
    const int calls_before = cs.data_calls.load();

    cs.credit = static_cast<std::size_t>(-1);
    conn->resume_read();
    CRPC_CHECK(wait_for([&] { return cs.size() == payload.size(); }, 5000));
    CRPC_CHECK_EQ(payload.size(), cs.size());
    (void)calls_before;
    l->stop();
}

CRPC_TEST(iocp_reader_bind_queues_no_lingering_wake) {
    // A bind over an empty receive buffer must not queue a delivery wake:
    // nothing can fulfil it, it would linger forever and later override an
    // unrelated pause, forcing delivery attempts the consumer never asked for.
    // Observable contract: the first rejected delivery attempt must be the
    // only one -- a lingering wake would surface as a second `on_data` call.
    iocp_backend server;
    iocp_backend client;

    plain_server ps;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ps);

    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ps.accepts.load() == 1; }, 5000));

    recording_source cs;
    cs.credit = 0;
    conn->bind_source(&cs); // empty rx: must queue nothing

    connection* server_conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(ps.m);
        if (!ps.bundles.empty()) {
            server_conn = ps.bundles.front()->conn.get();
        }
    }
    CRPC_CHECK(server_conn != nullptr);
    if (!server_conn) {
        return;
    }

    const std::string payload(200, 'x');
    counting_completion done;
    server_conn->sink().push(bytes_of(payload), &done);
    CRPC_CHECK(wait_for([&] { return done.calls.load() == 1; }, 5000));
    CRPC_CHECK(wait_for([&] { return cs.data_calls.load() >= 1; }, 5000));
    CRPC_CHECK_EQ(0u, cs.size());
    // Give any lingering-wake-driven extra attempt ample time to show up.
    std::this_thread::sleep_for(150ms);
    CRPC_CHECK_EQ(1, cs.data_calls.load());

    cs.credit = static_cast<std::size_t>(-1);
    conn->resume_read();
    CRPC_CHECK(wait_for([&] { return cs.size() == payload.size(); }, 5000));
    CRPC_CHECK_EQ(payload.size(), cs.size());
    l->stop();
}

CRPC_TEST(iocp_peer_close_eof_once) {
    iocp_backend server;
    iocp_backend client;

    plain_server ps;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ps);

    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ps.accepts.load() == 1; }, 5000));

    recording_source* ss = nullptr;
    CRPC_CHECK(wait_for([&] { ss = ps.first_source(); return ss != nullptr; }, 5000));
    if (!ss) {
        return;
    }

    conn->close();
    CRPC_CHECK(wait_for([&] { return ss->eof_calls.load() == 1; }, 5000));
    std::this_thread::sleep_for(milliseconds(30));
    CRPC_CHECK_EQ(1, ss->eof_calls.load());
    CRPC_CHECK_EQ(0, ss->error_calls.load());
    l->stop();
}

CRPC_TEST(iocp_reset_delivers_error_once) {
    iocp_backend server;

    plain_server ps;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ps);

    WSADATA wsa{};
    CRPC_CHECK_EQ(0, ::WSAStartup(MAKEWORD(2, 2), &wsa));
    SOCKET raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CRPC_CHECK(raw != INVALID_SOCKET);
    if (raw == INVALID_SOCKET) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CRPC_CHECK_EQ(0, ::connect(raw, reinterpret_cast<sockaddr*>(&addr),
                              static_cast<int>(sizeof(addr))));
    const char probe[] = "reset-me";
    (void)::send(raw, probe, static_cast<int>(sizeof(probe) - 1), 0);

    recording_source* ss = nullptr;
    CRPC_CHECK(wait_for([&] { ss = ps.first_source(); return ss != nullptr; }, 5000));

    linger lin{};
    lin.l_onoff = 1;
    lin.l_linger = 0;
    (void)::setsockopt(raw, SOL_SOCKET, SO_LINGER,
                       reinterpret_cast<const char*>(&lin),
                       static_cast<int>(sizeof(lin)));
    ::closesocket(raw);
    (void)::WSACleanup();

    if (!ss) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ss->error_calls.load() == 1; }, 5000));
    std::this_thread::sleep_for(milliseconds(30));
    CRPC_CHECK_EQ(1, ss->error_calls.load());
    CRPC_CHECK_EQ(0, ss->eof_calls.load());
    l->stop();
}

CRPC_TEST(iocp_100_concurrent_connections) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 4096;
    iocp_backend server(opt);
    iocp_backend client(opt);

    echo_server ss;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ss);

    const int n = 100;
    std::vector<std::unique_ptr<connection>> conns;
    std::vector<std::unique_ptr<recording_source>> sources;
    std::vector<std::unique_ptr<push_counter>> comps;
    conns.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto c = client.connect(endpoint::tcp("127.0.0.1", port),
                                connect_options{});
        CRPC_CHECK(c != nullptr);
        if (!c) {
            return;
        }
        auto s = std::unique_ptr<recording_source>(new recording_source());
        c->bind_source(s.get());
        sources.push_back(std::move(s));
        conns.push_back(std::move(c));
        comps.emplace_back(new push_counter());
    }

    CRPC_CHECK(wait_for([&] { return ss.accepts.load() == n; }, 15000));

    std::vector<std::string> tokens;
    for (int i = 0; i < n; ++i) {
        tokens.push_back("msg" + std::to_string(i));
        conns[static_cast<std::size_t>(i)]->sink().push(bytes_of(tokens.back()),
                                                        comps[static_cast<std::size_t>(i)].get());
    }
    for (int i = 0; i < n; ++i) {
        const std::size_t sz = tokens[static_cast<std::size_t>(i)].size();
        recording_source* s = sources[static_cast<std::size_t>(i)].get();
        CRPC_CHECK(wait_for([&] { return s->size() == sz; }, 5000));
        CRPC_CHECK_EQ(sz, s->size());
    }
    l->stop();
}

CRPC_TEST(iocp_close_during_inflight_write) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 8 * 1024 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);

    echo_server ss;
    ss.bind_source = false; // never reads: the client write must stay pending
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    // The listener is deliberately *not* started: the connection completes in
    // the kernel backlog, but the server never posts a receive, so the peer's
    // send buffer is the only sink for the write.

    connect_options co;
    co.send_buffer_bytes = 4096;
    co.recv_buffer_bytes = 4096;
    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), co);
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ss.accepts.load() == 0; }, 50));

    // The invariant under test is "exactly one completion", not "the write is
    // still pending". Whether a 128 KiB write is absorbed by the kernel before
    // the close depends on the loopback receive window and TCP autotuning,
    // which is not something a test may assume. So: observe whether it is still
    // in flight, then close, then require exactly one completion -- and require
    // that completion to be `canceled` precisely when it was still in flight.
    std::vector<std::byte> big(4 * 1024 * 1024, static_cast<std::byte>('z'));
    counting_completion done;
    conn->sink().push(bytes_of(big), &done);
    std::this_thread::sleep_for(milliseconds(60));
    const int calls_before_close = done.calls.load();
    CRPC_CHECK(calls_before_close <= 1);

    conn->close();
    CRPC_CHECK(wait_for([&] { return done.calls.load() == 1; }, 5000));
    CRPC_CHECK_EQ(1, done.calls.load());
    if (calls_before_close == 0) {
        // The write was still outstanding when the connection closed: the
        // backend must report cancellation rather than silently dropping it.
        CRPC_CHECK(done.canceled.load());
    }
    std::this_thread::sleep_for(milliseconds(40));
    CRPC_CHECK_EQ(1, done.calls.load());
    CRPC_CHECK_FALSE(conn->is_open());
    l->stop();
}

CRPC_TEST(iocp_10k_messages) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 64 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);

    echo_server ss;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ss);

    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(wait_for([&] { return ss.accepts.load() == 1; }, 5000));

    recording_source cs;
    conn->bind_source(&cs);

    // The backend has a bounded send queue, so the test may pipeline pushes as
    // long as the queue never exceeds its bound.  10000 * 4 bytes is 40000,
    // comfortably under the 64 KiB limit.
    const int total = 10000;
    const std::string msg = "ping";
    std::vector<std::unique_ptr<push_counter>> comps;
    comps.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        comps.emplace_back(new push_counter());
        conn->sink().push(bytes_of(msg), comps.back().get());
    }

    const std::size_t expected = static_cast<std::size_t>(total) * msg.size();
    CRPC_CHECK(wait_for([&] { return cs.size() >= expected; }, 30000));
    CRPC_CHECK_EQ(expected, cs.size());

    int calls = 0;
    int errors = 0;
    for (auto& c : comps) {
        calls += c->calls.load();
        errors += c->errors.load();
    }
    CRPC_CHECK_EQ(total, calls); // exactly one completion per push
    CRPC_CHECK_EQ(0, errors);
    CRPC_CHECK_EQ(0, cs.error_calls.load());
    l->stop();
}

CRPC_TEST(iocp_accept_burst_500) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 4096;
    iocp_backend server(opt);
    iocp_backend client(opt);

    echo_server ss;
    ss.bind_source = false;
    std::uint16_t port = 0;
    auto l = listen_free(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&ss);

    // The case is about accepting a burst, not about connect never failing: on a
    // loaded machine a single connect() out of 500 can transiently fail, and the old
    // version then failed on `established == 500` while accepts had correctly matched
    // every connection that did succeed (499). A failed connect is retried a bounded
    // number of times so the burst is still 500, and the assertion that matters --
    // accepts equals the number of established connections -- keeps its teeth.
    const int n = 500;
    std::vector<std::unique_ptr<connection>> conns;
    conns.reserve(static_cast<std::size_t>(n));
    int established = 0;
    int connect_failures = 0;
    for (int i = 0; i < n; ++i) {
        std::unique_ptr<connection> c;
        for (int attempt = 0; attempt < 3 && !c; ++attempt) {
            if (attempt != 0) {
                // Give the accept side a turn: a connect that failed while the
                // pre-posted AcceptEx batch was being replenished is backpressure.
                crpc_test::wait_for([] { return false; }, 10);
            }
            c = client.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
        }
        if (c) {
            ++established;
            conns.push_back(std::move(c));
        } else {
            ++connect_failures;
        }
    }
    if (connect_failures != 0) {
        std::printf("  connect failures after retries: %d\n", connect_failures);
    }
    CRPC_CHECK_EQ(n, established);
    // Every connection that was established must have been accepted: that is the
    // property under test, and it is independent of how many connects succeeded.
    CRPC_CHECK(wait_for([&] {
        return ss.accepts.load() == established;
    }, 30000));
    CRPC_CHECK_EQ(established, ss.accepts.load());
    l->stop();
}

void timer_cb(void* arg) noexcept;

CRPC_TEST(iocp_destruction_bounded_live_connection) {
    // Destroying a backend with a live connection, a started listener and an
    // armed timer must stop every owned thread promptly.  If any join blocked,
    // this bound would be exceeded (or the process would hang outright).
    std::unique_ptr<connection> conn;
    std::unique_ptr<listener> listener;
    {
        std::unique_ptr<iocp_backend> backend(new iocp_backend());
        CRPC_CHECK(backend->running());
        if (!backend->running()) {
            return;
        }
        echo_server ss;
        std::uint16_t port = 0;
        listener = listen_free(*backend, port);
        CRPC_CHECK(listener != nullptr);
        if (listener) {
            listener->start(&ss);
        }
        conn = backend->connect(endpoint::tcp("127.0.0.1", port),
                                connect_options{});
        CRPC_CHECK(conn != nullptr);
        if (conn) {
            CRPC_CHECK(wait_for([&] { return ss.accepts.load() == 1; }, 5000));
        }
        std::atomic<int> fired{0};
        (void)backend->arm_timer(seconds(10), &timer_cb, &fired);

        const auto start = steady_clock::now();
        backend.reset(); // stops pump + timer threads and force-closes
        const long long elapsed =
            duration_cast<milliseconds>(steady_clock::now() - start).count();
        CRPC_CHECK(elapsed < 2000);
        if (conn) {
            CRPC_CHECK_FALSE(conn->is_open());
        }
    }
    // Destroying the leftover handles after the backend must not touch it.
    conn.reset();
    listener.reset();
}


void timer_cb(void* arg) noexcept {
    static_cast<std::atomic<int>*>(arg)->fetch_add(1);
}

struct timer_probe {
    steady_clock::time_point start;
    std::atomic<long long> elapsed_ns{0};
    std::atomic<int> fired{0};
};

void probe_cb(void* arg) noexcept {
    auto* p = static_cast<timer_probe*>(arg);
    const auto now = steady_clock::now();
    p->elapsed_ns.store(duration_cast<nanoseconds>(now - p->start).count());
    p->fired.fetch_add(1);
}

CRPC_TEST(iocp_timer_fires_and_cancels) {
    iocp_backend backend;
    CRPC_CHECK(backend.running());
    if (!backend.running()) {
        return;
    }

    std::atomic<int> fired{0};
    const std::uint64_t id = backend.arm_timer(microseconds(500), &timer_cb,
                                               &fired);
    CRPC_CHECK(id != 0);
    CRPC_CHECK(wait_for([&] { return fired.load() == 1; }, 5000));
    std::this_thread::sleep_for(milliseconds(20));
    CRPC_CHECK_EQ(1, fired.load());

    std::atomic<int> fired2{0};
    const std::uint64_t id2 = backend.arm_timer(milliseconds(200), &timer_cb,
                                                &fired2);
    CRPC_CHECK(id2 != 0);
    CRPC_CHECK(backend.cancel_timer(id2));
    std::this_thread::sleep_for(milliseconds(400));
    CRPC_CHECK_EQ(0, fired2.load());

    timer_probe probe;
    probe.start = steady_clock::now();
    const std::uint64_t id3 =
        backend.arm_timer(microseconds(500), &probe_cb, &probe);
    CRPC_CHECK(id3 != 0);
    // The 5 s bound is the hang guard: it is the only deadline assertion left.
    CRPC_CHECK(wait_for([&] { return probe.fired.load() == 1; }, 5000));
    // This used to assert `elapsed < 20 ms`. That bound cannot see the degraded
    // path it was meant to guard: the pre-1803 fallback timer has ~15.6 ms
    // granularity, so it passes a 20 ms bound, while under load the
    // high-resolution path measured a 23.7-25.8 ms median and failed it. The
    // gate's direction was inverted and no wall-clock statistic can separate
    // the two paths.
    //
    // The capability is asserted positively: probe the OS first, so only a host
    // that cannot provide the high-resolution timer skips. A backend that runs
    // on such a host but never requested the flag must fail this check.
    HANDLE timer_probe_handle = ::CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    const bool os_supports = timer_probe_handle != nullptr;
    if (timer_probe_handle != nullptr) {
        ::CloseHandle(timer_probe_handle);
    }
    if (!os_supports) {
        crpc_test::skip("host predates Windows 10 1803: high-resolution "
                        "waitable timer unavailable");
    }
    CRPC_CHECK(backend.high_resolution_timer());
}


} // namespace

CRPC_TEST_MAIN()

#else

int main() { return 0; }

#endif // _WIN32


