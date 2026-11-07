// CRPC -- ping implementation: ICMP host probes and RPC PING/PONG probes.
//
// The ICMP side is a thin composition of the two primitives that already
// exist: `net::resolve_host` (name -> IPv4 literal, with the deadline shared by
// the probe) and `net::icmp_socket` (one echo request, platform-correct socket
// selection). Nothing about the ICMP wire format is re-implemented here.
//
// The RPC side delegates to `rpc::client::ping`, which stages a real PING frame
// on the connection (the server-side echo lives in src/server/server.cpp and
// the client-side echo in src/rpc/client.cpp). This file only maps that result
// into `ping_reply` and provides the executor/coroutine shapes.
#include <crpc/net/ping.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/net/address.hpp>
#include <crpc/net/icmp.hpp>
#include <crpc/net/ip.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/runtime.hpp>

namespace crpc {
namespace net {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string describe(rpc_code code, std::string_view message) {
    std::string text = to_string(code);
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    return text;
}

[[nodiscard]] ping_reply failure(rpc_code code, std::string message) {
    ping_reply reply;
    reply.code = code;
    reply.error = std::move(message);
    return reply;
}

[[nodiscard]] ping_reply from_error(const rpc_error& error) {
    ping_reply reply;
    reply.code = error.code;
    reply.error = describe(error.code, error.message);
    return reply;
}

[[nodiscard]] std::chrono::nanoseconds remaining(
    std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return std::chrono::nanoseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
}

// One process-wide counter: every probe opens its own socket, so the sequence
// only has to be non-zero and stable for the lifetime of that socket's wait.
[[nodiscard]] std::uint16_t next_icmp_sequence() noexcept {
    static std::atomic<std::uint32_t> counter{0};
    const std::uint32_t value =
        counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint16_t sequence = static_cast<std::uint16_t>(value & 0xFFFFu);
    return sequence == 0 ? std::uint16_t{1} : sequence;
}

// Resolves a name only when it is not already an IPv4 literal, so a caller
// that passes "127.0.0.1" never touches DNS. Mirrors the contract of
// `resolve_host`: a literal is answered without the resolver.
[[nodiscard]] result<ip_address> resolve_ipv4(
    std::string_view host, std::chrono::steady_clock::time_point deadline) {
    const result<ip_address> literal = ip_address::parse(host);
    if (literal.ok()) {
        return literal;
    }
    resolve_options options;
    options.family = ip_family::ipv4;
    options.timeout = remaining(deadline);
    result<std::vector<ip_address>> resolved = resolve_host(host, options);
    if (!resolved.ok()) {
        return resolved.error();
    }
    if (resolved.value().empty()) {
        return rpc_error{rpc_code::unavailable,
                         "the host resolved to no IPv4 address"};
    }
    return resolved.value().front();
}

// Shared state of a `_co` probe. The executor task publishes the reply and
// sets `done` under the mutex, then rings the shard; the coroutine parks and
// re-checks. Publishing before notifying is what rules out a lost wakeup: a
// notify that arrives before the coroutine parks is left as a semaphore token
// by `runtime::notify()`.
struct coro_wait_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    ping_reply reply;
};

[[nodiscard]] ping_reply run_on_executor(runtime& shard,
                                         async::executor& workers,
                                         std::function<ping_reply()> work) {
    auto state = std::make_shared<coro_wait_state>();
    auto submitted =
        workers.submit([state, &shard, work = std::move(work)]() {
            ping_reply reply = work();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->reply = std::move(reply);
                state->done = true;
            }
            state->cv.notify_all();
            shard.notify();
            return 0;
        });
    if (!submitted.valid()) {
        return failure(rpc_code::resource_exhausted,
                       "the executor refused the ping task");
    }

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->done) {
                return std::move(state->reply);
            }
        }
        const status parked = shard.park();
        if (!parked.ok()) {
            if (parked.code() == rpc_code::canceled) {
                return failure(
                    rpc_code::canceled,
                    "the shard stopped while the ping was waiting");
            }
            // `park()` reports `failed_precondition` outside a coroutine of
            // this runtime (and when corocc is not compiled in). The documented
            // fallback is the blocking wait; the probe itself is still bounded
            // by its timeout.
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] { return state->done; });
            return std::move(state->reply);
        }
    }
}

} // namespace

ping_reply ping_host_sync(std::string_view host,
                          std::chrono::nanoseconds timeout) {
    if (timeout < std::chrono::nanoseconds::zero()) {
        return failure(rpc_code::invalid_argument, "ping timeout is negative");
    }
    if (timeout == std::chrono::nanoseconds::zero()) {
        return failure(rpc_code::deadline_exceeded, "ping timeout is zero");
    }
    if (host.empty()) {
        return failure(rpc_code::invalid_argument, "ping host is empty");
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;

    result<ip_address> target = resolve_ipv4(host, deadline);
    if (!target.ok()) {
        return from_error(target.error());
    }
    if (!target.value().is_v4()) {
        return failure(rpc_code::invalid_argument,
                       "ICMP ping supports IPv4 targets only");
    }
    const std::chrono::nanoseconds budget = remaining(deadline);
    if (budget <= std::chrono::nanoseconds::zero()) {
        return failure(rpc_code::deadline_exceeded,
                       "the ping deadline elapsed during name resolution");
    }

    icmp_options options;
    options.mode = icmp_mode::automatic;
    options.timeout = budget;
    result<std::unique_ptr<icmp_socket>> opened = icmp_socket::open(target.value(),
                                                                    options);
    if (!opened.ok()) {
        return from_error(opened.error());
    }
    result<icmp_echo_reply> echo =
        opened.value()->echo(next_icmp_sequence(), remaining(deadline));
    if (!echo.ok()) {
        return from_error(echo.error());
    }

    ping_reply reply;
    reply.replied = true;
    reply.rtt = echo.value().rtt;
    reply.ttl = echo.value().ttl;
    return reply;
}

async::future<ping_reply> ping_host_async(async::executor& workers,
                                          std::string_view host,
                                          std::chrono::nanoseconds timeout) {
    // `submit` copies the callable into its queue, so the host must be owned.
    std::string owned(host);
    return workers.submit([host = std::move(owned), timeout]() {
        return ping_host_sync(host, timeout);
    });
}

ping_reply ping_host_co(runtime& shard, async::executor& workers,
                        std::string_view host,
                        std::chrono::nanoseconds timeout) {
    std::string owned(host);
    return run_on_executor(shard, workers,
                           [host = std::move(owned), timeout]() {
                               return ping_host_sync(host, timeout);
                           });
}

ping_reply rpc_ping_sync(rpc::client& connection,
                         std::chrono::nanoseconds timeout) {
    rpc::rpc_result<std::chrono::nanoseconds> measured =
        connection.ping(timeout);
    if (!measured.ok()) {
        return from_error(measured.error());
    }
    ping_reply reply;
    reply.replied = true;
    reply.rtt = measured.value();
    return reply;
}

async::future<ping_reply> rpc_ping_async(async::executor& workers,
                                         rpc::client& connection,
                                         std::chrono::nanoseconds timeout) {
    rpc::client* target = &connection;
    return workers.submit(
        [target, timeout]() { return rpc_ping_sync(*target, timeout); });
}

ping_reply rpc_ping_co(runtime& shard, async::executor& workers,
                       rpc::client& connection,
                       std::chrono::nanoseconds timeout) {
    rpc::client* target = &connection;
    return run_on_executor(
        shard, workers,
        [target, timeout]() { return rpc_ping_sync(*target, timeout); });
}

} // namespace net
} // namespace crpc
