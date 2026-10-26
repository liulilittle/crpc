// CRPC -- IOCP send-completion use-after-free regression.
//
// The IOCP backend borrows the `io::completion` handed to `sink::push`: the
// caller owns it and may destroy it as soon as `connection::close()` returns.
// The pump clears `send_inflight` at the top of `handle_send_complete`, *before*
// it runs the user callback.  If `close()` observes that cleared flag it skips
// the delivery wait, so it can return -- and the caller can free the
// completion -- while the pump is still inside (or about to enter) the callback.
// The callback then touches freed memory.
//
// Two cases drive that window:
//
//   * `iocp_close_racing_send_completion_delivery` is deterministic: the
//     completion signals the test the moment the pump enters `complete()` and
//     then yields, so `close()` is guaranteed to run while the pump is inside
//     the callback.  With the defect, `close()` returns at once, the test frees
//     the completion, and the pump's next access to it faults (the MSVC debug
//     heap fills the freed block with 0xDD, so the fault is
//     `mov rax,[rax+8]` with rax = 0xDDDDDDDDDDDDDDDD).  With the fix,
//     `close()` waits for the delivery and the completion is never touched
//     after it is freed.
//
//   * `iocp_close_racing_inflight_send` is the high-concurrency stress form:
//     a push is in flight while a second thread closes the connection and frees
//     the completion the instant `close()` returns, many times, optionally
//     under background load.  It is the shape the distributed TCP suite hit.
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
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace std::chrono;

std::uint16_t uaf_next_port() {
    static std::mutex m;
    static unsigned counter = 0;
    std::lock_guard<std::mutex> lk(m);
    const unsigned base = 24000u + (::GetCurrentProcessId() % 16000u);
    const unsigned value = base + counter;
    ++counter;
    return static_cast<std::uint16_t>(value % 56000u + 1024u);
}

span<const std::byte> uaf_bytes(const std::vector<std::byte>& v) {
    return span<const std::byte>(v.data(), v.size());
}

// A server source that drains everything and never writes, so the client's
// sends complete (the receive window keeps moving) and the send-completion
// path is exercised.
struct uaf_drain_source final : io::source {
    std::atomic<std::size_t> bytes{0};
    std::size_t on_data(span<const std::byte> data) noexcept override {
        bytes.fetch_add(data.size());
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct uaf_server final : accept_sink {
    std::mutex m;
    std::vector<std::unique_ptr<connection>> conns;
    std::vector<std::unique_ptr<uaf_drain_source>> sources;
    std::atomic<int> accepts{0};

    void on_accept(std::unique_ptr<connection> c) noexcept override {
        std::unique_ptr<uaf_drain_source> s(new (std::nothrow) uaf_drain_source());
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

// Completion used by the deterministic case.  It publishes `entered` the moment
// the pump calls `complete()` and then yields, which holds the pump inside the
// callback while the test runs `close()` on another thread.  The yield is a
// test device: it makes the lifetime window observable without changing the
// backend's behaviour.
struct uaf_gate_completion final : io::completion {
    std::atomic<bool> entered{false};
    std::atomic<int> calls{0};

    void complete(io::io_result) noexcept override {
        entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(milliseconds(30));
        calls.fetch_add(1, std::memory_order_release);
    }
};

// Completion used by the stress case: heap allocated and freed by the test the
// moment `close()` returns, exactly what a real client does with its per-write
// completion when it tears the connection down.
struct uaf_completion final : io::completion {
    std::atomic<int> calls{0};
    void complete(io::io_result) noexcept override { calls.fetch_add(1); }
};

// Background load: a few spinning threads make the scheduler preempt the pump
// thread right after it releases the connection lock, which is the window the
// defect needs.
struct uaf_load_pool {
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;

    void start(int n) {
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([this] {
                volatile std::uint64_t x = 0;
                while (!stop.load(std::memory_order_relaxed)) {
                    x += 1;
                }
                (void)x;
            });
        }
    }
    ~uaf_load_pool() {
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
};

CRPC_TEST(iocp_close_racing_send_completion_delivery) {
    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 1024 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);
    CRPC_CHECK(server.running());
    CRPC_CHECK(client.running());

    uaf_server us;
    const std::uint16_t port = uaf_next_port();
    auto l = server.listen(endpoint::tcp("127.0.0.1", port), listen_options{});
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

    uaf_gate_completion* comp = new (std::nothrow) uaf_gate_completion();
    CRPC_CHECK(comp != nullptr);
    if (comp == nullptr) {
        return;
    }

    std::vector<std::byte> payload(64 * 1024, static_cast<std::byte>('q'));
    conn->sink().push(uaf_bytes(payload), comp);

    // Wait until the pump is inside the completion callback.  At that instant
    // `send_inflight` is already false but the delivery is not finished, so a
    // close() here must wait for the callback to return.
    CRPC_CHECK(crpc_test::wait_for([&] { return comp->entered.load(); }, 5000));

    conn->close();

    // The completion was delivered exactly once, before close() returned.
    CRPC_CHECK_EQ(1, comp->calls.load());
    delete comp;
    // Give the pump a chance to touch the freed object if the delivery wait was
    // skipped: without the fix the pump wakes here and faults on the freed
    // completion; with the fix the completion was already delivered.
    std::this_thread::sleep_for(milliseconds(120));
    conn.reset();
    l->stop();
}

CRPC_TEST(iocp_close_racing_inflight_send) {
    const char* iters_env = std::getenv("CRPC_IOCP_UAF_ITERS");
    const int iters = iters_env != nullptr ? std::atoi(iters_env) : 400;
    const char* load_env = std::getenv("CRPC_IOCP_UAF_LOAD");
    const int load = load_env != nullptr ? std::atoi(load_env) : 0;

    iocp_options opt;
    opt.recv_buffer_bytes = 4096;
    opt.send_queue_bytes = 1024 * 1024;
    iocp_backend server(opt);
    iocp_backend client(opt);
    CRPC_CHECK(server.running());
    CRPC_CHECK(client.running());

    uaf_server us;
    const std::uint16_t port = uaf_next_port();
    auto l = server.listen(endpoint::tcp("127.0.0.1", port), listen_options{});
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&us);

    uaf_load_pool lp;
    if (load > 0) {
        lp.start(load);
    }

    int clean = 0;
    for (int i = 0; i < iters; ++i) {
        connect_options co;
        co.send_buffer_bytes = 4096;
        co.recv_buffer_bytes = 4096;
        auto conn = client.connect(endpoint::tcp("127.0.0.1", port), co);
        if (!conn) {
            continue;
        }
        if (!crpc_test::wait_for([&] { return us.accepts.load() >= 1; }, 5000)) {
            break;
        }

        uaf_completion* comp = new (std::nothrow) uaf_completion();
        CRPC_CHECK(comp != nullptr);
        if (comp == nullptr) {
            break;
        }

        // A frame large enough to stay in flight across the loopback window but
        // small enough to complete promptly; the send and the close race.
        std::vector<std::byte> payload(64 * 1024, static_cast<std::byte>('q'));
        conn->sink().push(uaf_bytes(payload), comp);

        // Close from another thread while the send is in flight.  The closer
        // returns as soon as the backend lets it; the completion is freed then.
        std::thread closer([&conn] { conn->close(); });
        closer.join();

        // Exactly one completion, delivered before close() returned.
        CRPC_CHECK_EQ(1, comp->calls.load());
        delete comp;
        ++clean;
        conn.reset();
    }

    CRPC_CHECK(clean >= 1);
    l->stop();
}

} // namespace

CRPC_TEST_MAIN()

#else

int main() { return 0; }

#endif // _WIN32
