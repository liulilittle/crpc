// CRPC -- IOCP send-completion lifetime: nested push from a completion callback.
//
// `rpc::client::on_write_complete` runs inside the backend's send completion and
// calls `flush_write()`, which calls `sink::push` again on the *same*
// connection.  The IOCP backend sets `send_delivery_done = true` after the
// callback loop in `handle_send_complete`, which overwrites the `false` that the
// nested `post_send` stored.  A concurrent `close()` therefore observes
// "delivery done" while the nested WSASend is still in flight, returns, and the
// caller may free the nested completion -- the pump then touches freed memory.
//
// This is an adversarial regression for that reentrant-push ordering.  It is
// deliberately separate from `test_io_iocp_uaf.cpp`, which only covers a
// completion that does not push again.
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
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace std::chrono;

// Candidate ports must stay clear of the Windows dynamic/ephemeral range
// (49152..65535): this process's own client connections and every other
// parallel ctest process allocate ephemeral ports there, so a bind inside
// that range collides under `ctest -j8` (observed: listen() returned null and
// the test failed without touching anything it was written to test). The
// 20000..29999 band is below the ephemeral floor; the PID keeps parallel
// processes apart and the counter separates repeated calls in one process.
std::uint16_t nested_next_port() {
    static std::mutex m;
    static unsigned counter = 0;
    std::lock_guard<std::mutex> lk(m);
    const unsigned base = 20000u + (::GetCurrentProcessId() % 5000u);
    const unsigned value = base + counter;
    ++counter;
    return static_cast<std::uint16_t>(20000u + value % 10000u);
}

// Binds the first free candidate port, retrying up to 16 times. A computed
// port is never guaranteed to be free: another process -- including a
// parallel ctest test -- can take it between our pick and our bind. The
// `listener` interface exposes no local address (io.hpp), so the test cannot
// bind port 0 and read the assignment back; walking a candidate list is the
// deterministic alternative. `bound_port` receives the port that actually
// bound; callers must connect to it, not to the first pick.
std::unique_ptr<listener> listen_with_retry(iocp_backend& server,
                                            std::uint16_t& bound_port) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint16_t port = nested_next_port();
        auto l = server.listen(endpoint::tcp("127.0.0.1", port),
                               listen_options{});
        if (l != nullptr) {
            bound_port = port;
            return l;
        }
    }
    std::fprintf(stderr,
                 "nested_push: all 16 candidate ports failed to bind\n");
    std::fflush(stderr);
    return nullptr;
}

span<const std::byte> nested_bytes(const std::vector<std::byte>& v) {
    return span<const std::byte>(v.data(), v.size());
}

struct nested_drain_source final : io::source {
    std::atomic<std::size_t> bytes{0};
    std::size_t on_data(span<const std::byte> data) noexcept override {
        bytes.fetch_add(data.size());
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct nested_server final : accept_sink {
    std::mutex m;

    // Declaration order is the destruction order reversed, and it matters here.
    // A connection holds a *borrowed* pointer to its source (`bind_source`), and
    // `~iocp_connection` calls `bound.retire()` to wait for any `on_data` that is
    // still running. That guarantee only helps if the source is still alive when
    // the connection dies -- so `sources` must be declared BEFORE `conns`, which
    // makes `conns` destruct first and `sources` last.
    //
    // The reverse order (conns first, sources second) is what this test used to
    // have, and it produced a real 0xC0000005: `sources` were freed, then the
    // connection destructor waited for a delivery that was already inside a
    // virtual call on the freed source. Reproduced at roughly 1 in 40 runs under
    // CPU load, faulting at `iocp_backend.cpp:746` (`s->on_data(...)`).
    std::vector<std::unique_ptr<nested_drain_source>> sources;
    std::vector<std::unique_ptr<connection>> conns;

    std::atomic<int> accepts{0};

    void on_accept(std::unique_ptr<connection> c) noexcept override {
        std::unique_ptr<nested_drain_source> s(new (std::nothrow) nested_drain_source());
        if (!s) {
            return;
        }
        c->bind_source(s.get());
        std::lock_guard<std::mutex> lk(m);
        conns.push_back(std::move(c));
        sources.push_back(std::move(s));
        accepts.fetch_add(1);
    }
};

struct nested_inner final : io::completion {
    std::atomic<bool> entered{false};
    std::atomic<int> calls{0};
    void complete(io::io_result) noexcept override {
        entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(milliseconds(20));
        calls.fetch_add(1, std::memory_order_release);
    }
};

// Pushes the second chunk from inside the first chunk's completion callback,
// exactly the shape `rpc::client::on_write_complete -> flush_write` produces.
struct nested_outer final : io::completion {
    io::sink* sink = nullptr;
    io::completion* inner = nullptr;
    span<const std::byte> tail;
    std::atomic<bool> pushed{false};
    std::atomic<int> calls{0};

    void complete(io::io_result) noexcept override {
        calls.fetch_add(1, std::memory_order_release);
        if (sink != nullptr && inner != nullptr) {
            sink->push(tail, inner);
        }
        pushed.store(true, std::memory_order_release);
    }
};

// The production `rpc::client` reuses one `write_completion` member for every
// push, so the nested push re-arms the *same* borrowed object while the outer
// handler still holds it in its outcome list.  This mirrors that exactly.
struct nested_same final : io::completion {
    io::sink* sink = nullptr;
    span<const std::byte> tail;
    std::atomic<bool> pushed{false};
    std::atomic<int> calls{0};

    void complete(io::io_result) noexcept override {
        const int n = calls.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n == 1 && sink != nullptr) {
            sink->push(tail, this);
        }
        pushed.store(true, std::memory_order_release);
    }
};

CRPC_TEST(iocp_close_waits_for_nested_push_delivery) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 1024 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);
    CRPC_CHECK(server.running());
    CRPC_CHECK(client.running());

    nested_server us;
    std::uint16_t port = 0;
    auto l = listen_with_retry(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&us);

    connect_options co;
    co.send_buffer_bytes = 4096;
    co.recv_buffer_bytes = 4096;
    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), co);
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(crpc_test::wait_for([&] { return us.accepts.load() >= 1; }, 5000));

    nested_inner* inner = new (std::nothrow) nested_inner();
    CRPC_CHECK(inner != nullptr);
    if (inner == nullptr) {
        return;
    }
    nested_outer outer;
    std::vector<std::byte> first(256, static_cast<std::byte>('a'));
    std::vector<std::byte> tail(64 * 1024, static_cast<std::byte>('b'));
    outer.sink = &conn->sink();
    outer.inner = inner;
    outer.tail = nested_bytes(tail);

    conn->sink().push(nested_bytes(first), &outer);
    CRPC_CHECK(crpc_test::wait_for([&] { return outer.pushed.load(); }, 5000));

    // The outer callback has already posted the nested send.  A close() that
    // returns here must have waited for the nested completion too: the caller
    // frees it as soon as close() returns.
    conn->close();
    const int at_close = inner->calls.load();

    const bool free_early = std::getenv("CRPC_IOCP_NESTED_FREE") != nullptr;
    if (free_early && at_close == 0) {
        delete inner;
        inner = nullptr;
        std::this_thread::sleep_for(milliseconds(400));
    }

    // Correct contract: the nested completion ran before close() returned.
    CRPC_CHECK_EQ(1, at_close);
    if (inner != nullptr) {
        (void)crpc_test::wait_for([&] { return inner->calls.load() == 1; }, 3000);
        delete inner;
    }
    conn.reset();
    l->stop();
}

CRPC_TEST(iocp_close_waits_for_reused_completion_repush) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 1024 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);
    CRPC_CHECK(server.running());
    CRPC_CHECK(client.running());

    nested_server us;
    std::uint16_t port = 0;
    auto l = listen_with_retry(server, port);
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&us);

    connect_options co;
    co.send_buffer_bytes = 4096;
    co.recv_buffer_bytes = 4096;
    auto conn = client.connect(endpoint::tcp("127.0.0.1", port), co);
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    CRPC_CHECK(crpc_test::wait_for([&] { return us.accepts.load() >= 1; }, 5000));

    nested_same* comp = new (std::nothrow) nested_same();
    CRPC_CHECK(comp != nullptr);
    if (comp == nullptr) {
        return;
    }
    std::vector<std::byte> first(256, static_cast<std::byte>('a'));
    std::vector<std::byte> tail(64 * 1024, static_cast<std::byte>('b'));
    comp->sink = &conn->sink();
    comp->tail = nested_bytes(tail);

    conn->sink().push(nested_bytes(first), comp);
    CRPC_CHECK(crpc_test::wait_for([&] { return comp->pushed.load(); }, 5000));

    conn->close();
    const int at_close = comp->calls.load();

    const bool free_early = std::getenv("CRPC_IOCP_NESTED_FREE") != nullptr;
    if (free_early && at_close < 2) {
        delete comp;
        comp = nullptr;
        std::this_thread::sleep_for(milliseconds(400));
    }

    // The same object was pushed twice: both deliveries must precede close().
    CRPC_CHECK_EQ(2, at_close);
    if (comp != nullptr) {
        (void)crpc_test::wait_for([&] { return comp->calls.load() == 2; }, 3000);
        delete comp;
    }
    conn.reset();
    l->stop();
}

} // namespace

CRPC_TEST_MAIN()

#else

// Off Windows this binary is a no-op: IOCP does not exist here. It is still
// built (and registered with CTest) so the test list stays identical across
// platforms, but it executes nothing. A sanitizer job therefore provides
// *zero* coverage for the Windows IOCP paths in this file -- do not cite a
// green asan/tsan run of this binary as evidence about them. (Recorded after
// making exactly that mistake: an asan run "passed" 30/30 because main()
// returned immediately.)

int main() { return 0; }

#endif // _WIN32
