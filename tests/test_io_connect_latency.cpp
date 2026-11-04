// CRPC -- IOCP connect latency regression (runs alone; see RUN_SERIAL).
//
// `iocp_backend::do_connect` used to confirm a non-blocking handshake with
// `getsockopt(SOL_SOCKET, SO_ERROR)` after `select` reported the socket
// writable. On Windows that call was measured at p90 ~14.9 ms on roughly a third
// of loopback connects -- the default timer resolution is ~15.6 ms -- while
// every other call in the same loop stayed under 0.5 ms. So every new connection
// paid up to a full tick for a check that `select`'s exception set already
// answers, and a deployment that opens connections per request paid it
// constantly.
//
// Measurements that isolated it (each against a plain Winsock listener, so crpc
// was not involved in the control):
//
//   select + SO_ERROR          p50 0.42 ms  p90 15.19 ms
//   select only                p50 0.09 ms  p90 0.14 ms   <- zero samples > 5 ms
//   WSAEventSelect(FD_CONNECT) p50 0.10 ms  p90 0.17 ms   <- zero samples > 5 ms
//   WSAPoll(POLLOUT) + SO_ERROR p50 0.50 ms p90 14.92 ms
//   blocking connect           p50 0.08 ms  p90 0.11 ms
//
// The exception set was then verified to be a complete failure signal on its
// own: 250/250 connects to a live listener succeeded and 250/250 to a closed
// port were reported as failures, with zero false successes either way.
//
// Why this is its own binary and marked RUN_SERIAL
// ------------------------------------------------
// Two earlier instruments for this same regression were wrong, and both were
// caught by measurement:
//
//   * A wall-clock limit ("fewer than 2 of 200 connects over 8 ms"). Under
//     `ctest -j24` it went red on a *correct* build -- 8 of 200 over the limit,
//     36 ms worst -- because this process was competing for the machine. A test
//     that fails on a correct build is worse than no test.
//   * Process CPU time (`GetProcessTimes`). It is process-wide, so it includes
//     the server backend's pump threads: 508 us per connect fixed, 859 us broken.
//     It separates the two, but by a factor that depends on how the threads
//     interleave.
//
// A latency assertion is only meaningful when the machine is not saturated by
// 24 sibling test binaries, so the case lives in its own executable and
// tests/CMakeLists.txt sets RUN_SERIAL on it. That is a deliberate, stated scope
// limit rather than a tolerance widened until the flake disappeared.
//
// What the assertion is: p90 of 200 connects. Measured with the defect
// reinstated median 0.44 ms / p90 15.1 ms; fixed, median 0.21 ms / p90 0.28 ms.
// Only about a third of connects take the tick, so the median barely moves --
// p90 is the statistic that separates them, by a factor of fifty.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <crpc/io.hpp>

#if defined(_WIN32)

#include <crpc/io/iocp_backend.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

// <winsock2.h> must precede <windows.h>, which the harness pulls in.
#include <winsock2.h>

#include "ephemeral_ports.hpp"
#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace std::chrono;

// Ports below the Windows dynamic range (49152+), separated per process so a
// parallel ctest run cannot collide.
std::uint16_t latency_next_port() {
    static std::mutex m;
    static unsigned counter = 0;
    std::lock_guard<std::mutex> lk(m);
    const unsigned base = 20000u + (::GetCurrentProcessId() % 5000u);
    const unsigned value = base + counter;
    ++counter;
    return static_cast<std::uint16_t>(20000u + value % 10000u);
}

// Accepts every connection and holds it. This case measures connect(), not
// throughput, so the server side only has to keep the accept queue moving.
struct hold_server final : accept_sink {
    std::mutex m;
    std::vector<std::unique_ptr<connection>> conns;
    std::atomic<int> accepts{0};

    void on_accept(std::unique_ptr<connection> c) noexcept override {
        c->bind_source(nullptr);
        {
            std::lock_guard<std::mutex> lk(m);
            conns.push_back(std::move(c));
        }
        accepts.fetch_add(1, std::memory_order_relaxed);
    }
};

int percentile_index(std::size_t n, double q) {
    if (n == 0) {
        return -1;
    }
    const auto raw = static_cast<std::size_t>(q * static_cast<double>(n - 1));
    return static_cast<int>(raw < n ? raw : n - 1);
}

CRPC_TEST(iocp_connect_does_not_pay_a_timer_tick) {
    iocp_backend server_backend;
    iocp_backend client_backend;
    CRPC_CHECK(server_backend.running());
    CRPC_CHECK(client_backend.running());

    hold_server server;
    std::uint16_t port = 0;
    std::unique_ptr<listener> l;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint16_t candidate = latency_next_port();
        l = server_backend.listen(endpoint::tcp("127.0.0.1", candidate),
                                  listen_options{});
        if (l != nullptr) {
            port = candidate;
            break;
        }
    }
    CRPC_CHECK(l != nullptr);
    if (!l) {
        return;
    }
    l->start(&server);

    // A run needs 30 warm-up + 200 measured connects, each parking a source
    // port in TIME_WAIT. Checked once, before the first connect, so the guard
    // cannot itself be starved by the warm-up loop; see ephemeral_ports.hpp
    // for why the margin is 4096 rather than this run's 230.
    const crpc_test::ephemeral_port_state before =
        crpc_test::probe_ephemeral_ports();
    if (crpc_test::ephemeral_ports_scarce(before)) {
        std::printf(
            "  SKIP connect latency: only %d of %d ephemeral ports free "
            "(%d in TIME_WAIT).%s",
            before.free_ports, before.range, before.time_wait,
            " Repeating this case in a loop drains the local port range, "
            "which is an OS limit rather than a latency result.\n");
        l->stop();
        crpc_test::skip("ephemeral ports exhausted");
    }

    // Warm up: first-touch allocation and provider setup are not what this
    // measures.
    for (int i = 0; i < 30; ++i) {
        auto c = client_backend.connect(endpoint::tcp("127.0.0.1", port),
                                        connect_options{});
        if (!c) {
            const crpc_test::ephemeral_port_state now =
                crpc_test::probe_ephemeral_ports();
            std::printf(
                "  warm-up connect #%d failed (%d of %d ephemeral ports "
                "free, %d in TIME_WAIT), so the measurement cannot run\n",
                i, now.free_ports, now.range, now.time_wait);
            l->stop();
            crpc_test::skip("port range exhausted during warm-up");
        }
        c->bind_source(nullptr);
        c->close();
    }

    constexpr std::size_t kN = 200;
    std::vector<double> samples;
    samples.reserve(kN);
    int connect_failures = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        const auto began = steady_clock::now();
        auto c = client_backend.connect(endpoint::tcp("127.0.0.1", port),
                                        connect_options{});
        const double dt = duration<double, std::milli>(steady_clock::now() - began)
                              .count();
        if (!c) {
            // A refusal here is the OS refusing the connection, not a latency
            // sample; report the port table alongside it so the cause is
            // visible rather than guessed at.
            const crpc_test::ephemeral_port_state now =
                crpc_test::probe_ephemeral_ports();
            std::printf(
                "  connect #%zu failed after %.1f ms (%d of %d ephemeral ports "
                "free, %d in TIME_WAIT)\n",
                i, dt, now.free_ports, now.range, now.time_wait);
            ++connect_failures;
            break;
        }
        samples.push_back(dt);
        c->bind_source(nullptr);
        c->close();
    }

    CRPC_CHECK(connect_failures == 0);
    CRPC_CHECK(samples.size() >= kN / 2);
    if (samples.size() < kN / 2) {
        l->stop();
        return;
    }

    std::sort(samples.begin(), samples.end());
    const int mi = percentile_index(samples.size(), 0.50);
    const int p90i = percentile_index(samples.size(), 0.90);
    const double median = samples[static_cast<std::size_t>(mi)];
    const double p90 = samples[static_cast<std::size_t>(p90i)];
    std::printf("  connect over %zu: median=%.3f ms p90=%.3f ms max=%.3f ms\n",
                samples.size(), median, p90, samples.back());

    // Fixed:  median ~0.2 ms, p90 ~0.3 ms.
    // Broken: median ~0.4 ms, p90 ~15 ms (both measured).
    // The median bound is a loose sanity check; p90 is the gate, sitting twenty
    // times below the broken value and fifteen times above the fixed one.
    CRPC_CHECK(median < 5.0);
    CRPC_CHECK(p90 < 7.5);

    l->stop();
}

} // namespace

CRPC_TEST_MAIN()

#else

// Off Windows this binary is a no-op: IOCP does not exist here. It is still
// built and registered with CTest so the test list stays identical across
// platforms, but it executes nothing.

int main() {
    std::printf("SKIP crpc_test_io_connect_latency: IOCP does not exist on this platform; this binary executes nothing\n");
    return 0;
}

#endif // _WIN32
