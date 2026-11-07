// CRPC -- ping API: real ICMP loopback probes and RPC PING/PONG round trips.
//
// The ICMP cases open a real socket and send real echo requests to 127.0.0.1,
// exactly like test_net_icmp.cpp. They skip with a visible reason where the
// environment refuses ICMP (non-admin Windows, Linux without ping_group_range
// and without CAP_NET_RAW); everything else is a defect.
//
// The RPC cases use an inproc server/client pair: the server's PING echo is
// the real protocol code path (src/server/server.cpp), so a mismatched token
// or a wrong reply type is caught here rather than in a mock.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <crpc/async/executor.hpp>
#include <crpc/io.hpp>
#include <crpc/net/ping.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

constexpr std::chrono::nanoseconds k_loopback_timeout{2s};
constexpr std::chrono::nanoseconds k_rpc_blackhole_timeout{150ms};

// True when the environment refused the ICMP socket rather than the probe
// failing. The caller then skips the case with the library's own message.
[[nodiscard]] bool icmp_denied(const crpc::net::ping_reply& reply) {
    return !reply.replied && reply.code == crpc::rpc_code::permission_denied;
}

void skip_icmp_if_denied(const crpc::net::ping_reply& reply) {
    if (icmp_denied(reply)) {
        crpc_test::skip(reply.error.empty()
                            ? "ICMP requires privileges in this environment"
                            : reply.error.c_str());
    }
}

// --- inproc RPC pair ---------------------------------------------------------

// A real server plus one low-level client over the inproc backend. No facade:
// the ping API is defined on `rpc::client`, and the facade hides its pooled
// low-level connections.
struct rpc_pair {
    crpc::inproc_backend backend;
    std::unique_ptr<crpc::server> server;
    std::unique_ptr<crpc::rpc::client> client;
    std::string name;

    explicit rpc_pair(std::string endpoint_name) : name(std::move(endpoint_name)) {}

    ~rpc_pair() {
        if (client) {
            client->close();
        }
        if (server) {
            server->stop();
        }
    }

    [[nodiscard]] bool start() {
        crpc::server::builder sb;
        sb.add_listener(crpc::endpoint::inproc(name), backend);
        server = sb.build();
        if (!server) {
            return false;
        }
        if (!server->start().ok()) {
            return false;
        }
        auto conn =
            backend.connect(crpc::endpoint::inproc(name), crpc::connect_options{});
        if (!conn) {
            return false;
        }
        client = std::make_unique<crpc::rpc::client>(std::move(conn));
        return true;
    }
};

// A transport that accepts writes and never delivers anything: the PING is
// staged and flushed successfully, but no PONG can ever arrive. It exists to
// prove the timeout path terminates instead of hanging a waiter forever.
class blackhole_connection final : public crpc::connection {
public:
    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open_.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("ping-blackhole");
    }

private:
    struct silent_source final : crpc::io::source {
        std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };
    struct swallowing_sink final : crpc::io::sink {
        void push(crpc::span<const std::byte>, crpc::io::completion*) noexcept override {}
    };

    silent_source source_;
    swallowing_sink sink_;
    std::atomic<bool> open_{true};
};

// --- coroutine drivers -------------------------------------------------------

#if defined(CRPC_HAVE_COROCC)

struct icmp_coro_job {
    crpc::runtime* shard = nullptr;
    crpc::async::executor* pool = nullptr;
    std::atomic<bool> done{false};
    crpc::net::ping_reply reply;
};

void icmp_coro_entry(void* ctx) {
    auto* job = static_cast<icmp_coro_job*>(ctx);
    job->reply = crpc::net::ping_host_co(*job->shard, *job->pool, "127.0.0.1",
                                         k_loopback_timeout);
    job->done.store(true, std::memory_order_release);
}

struct rpc_coro_job {
    crpc::runtime* shard = nullptr;
    crpc::async::executor* pool = nullptr;
    crpc::rpc::client* client = nullptr;
    std::atomic<bool> done{false};
    crpc::net::ping_reply reply;
};

void rpc_coro_entry(void* ctx) {
    auto* job = static_cast<rpc_coro_job*>(ctx);
    job->reply = crpc::net::rpc_ping_co(*job->shard, *job->pool, *job->client,
                                        k_loopback_timeout);
    job->done.store(true, std::memory_order_release);
}

#endif // CRPC_HAVE_COROCC

} // namespace

// --- ICMP -------------------------------------------------------------------

CRPC_TEST(net_ping_icmp_sync_loopback) {
    const crpc::net::ping_reply reply =
        crpc::net::ping_host_sync("127.0.0.1", k_loopback_timeout);
    skip_icmp_if_denied(reply);
    CRPC_CHECK(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, reply.code);
    CRPC_CHECK(reply.error.empty());
    CRPC_CHECK(reply.rtt > std::chrono::nanoseconds{0});
    CRPC_CHECK(reply.rtt < k_loopback_timeout);
#if defined(CRPC_OS_WINDOWS)
    // The raw-socket receive path carries the IPv4 header, so TTL is present.
    CRPC_CHECK(reply.ttl > 0);
#endif
}

CRPC_TEST(net_ping_icmp_async_loopback) {
    crpc::async::executor_options options;
    options.threads = 2;
    options.queue_capacity = 16;
    crpc::async::executor pool(options);
    auto future =
        crpc::net::ping_host_async(pool, "127.0.0.1", k_loopback_timeout);
    CRPC_CHECK(future.valid());
    if (!future.valid()) {
        return;
    }
    const auto waited = future.wait_for(10s);
    CRPC_CHECK(waited.ok());
    if (!waited.ok()) {
        return;
    }
    const crpc::net::ping_reply& reply = waited.value();
    skip_icmp_if_denied(reply);
    CRPC_CHECK(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, reply.code);
    CRPC_CHECK(reply.rtt > std::chrono::nanoseconds{0});
    CRPC_CHECK(reply.rtt < k_loopback_timeout);
}

CRPC_TEST(net_ping_icmp_zero_timeout_is_immediate) {
    const auto started = std::chrono::steady_clock::now();
    const crpc::net::ping_reply reply =
        crpc::net::ping_host_sync("127.0.0.1", std::chrono::nanoseconds{0});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::deadline_exceeded, reply.code);
    CRPC_CHECK_FALSE(reply.error.empty());
    CRPC_CHECK(elapsed < 1s);
}

CRPC_TEST(net_ping_icmp_invalid_host_fails_clearly) {
    const auto started = std::chrono::steady_clock::now();
    const crpc::net::ping_reply reply =
        crpc::net::ping_host_sync("", 100ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK(reply.code != crpc::rpc_code::ok);
    CRPC_CHECK_FALSE(reply.error.empty());
    CRPC_CHECK(elapsed < 1s);
}

CRPC_TEST(net_ping_icmp_negative_timeout_rejected) {
    const crpc::net::ping_reply reply =
        crpc::net::ping_host_sync("127.0.0.1", -1ms);
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, reply.code);
    CRPC_CHECK_FALSE(reply.error.empty());
}

CRPC_TEST(net_ping_icmp_co_loopback) {
#if !defined(CRPC_HAVE_COROCC)
    crpc_test::skip("requires CRPC_HAVE_COROCC (the coroutine form is corocc-only)");
#else
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    crpc::async::executor_options options;
    options.threads = 2;
    options.queue_capacity = 16;
    crpc::async::executor pool(options);

    icmp_coro_job job;
    job.shard = &shard;
    job.pool = &pool;
    CRPC_CHECK(shard.spawn(&icmp_coro_entry, &job));
    const bool finished = crpc_test::wait_for(
        [&] { return job.done.load(std::memory_order_acquire); }, 10000);

    shard.stop();
    shard.join();

    CRPC_CHECK(finished);
    if (!finished) {
        return;
    }
    skip_icmp_if_denied(job.reply);
    CRPC_CHECK(job.reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, job.reply.code);
    CRPC_CHECK(job.reply.rtt > std::chrono::nanoseconds{0});
    CRPC_CHECK(job.reply.rtt < k_loopback_timeout);
#endif
}

// --- RPC --------------------------------------------------------------------

CRPC_TEST(net_ping_rpc_sync_loopback) {
    rpc_pair pair("net-ping-rpc-sync");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }
    const crpc::net::ping_reply reply =
        crpc::net::rpc_ping_sync(*pair.client, k_loopback_timeout);
    CRPC_CHECK(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, reply.code);
    CRPC_CHECK(reply.error.empty());
    CRPC_CHECK(reply.rtt >= std::chrono::nanoseconds{0});
    CRPC_CHECK(reply.rtt < k_loopback_timeout);
    // TTL is an ICMP-only field; the RPC form documents it as zero.
    CRPC_CHECK_EQ(std::uint8_t{0}, reply.ttl);
}

CRPC_TEST(net_ping_rpc_async_loopback) {
    rpc_pair pair("net-ping-rpc-async");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }
    crpc::async::executor_options options;
    options.threads = 2;
    options.queue_capacity = 16;
    crpc::async::executor pool(options);
    auto future =
        crpc::net::rpc_ping_async(pool, *pair.client, k_loopback_timeout);
    CRPC_CHECK(future.valid());
    if (!future.valid()) {
        return;
    }
    const auto waited = future.wait_for(10s);
    CRPC_CHECK(waited.ok());
    if (!waited.ok()) {
        return;
    }
    CRPC_CHECK(waited.value().replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, waited.value().code);
    CRPC_CHECK(waited.value().rtt >= std::chrono::nanoseconds{0});
    CRPC_CHECK(waited.value().rtt < k_loopback_timeout);
}

CRPC_TEST(net_ping_rpc_co_loopback) {
#if !defined(CRPC_HAVE_COROCC)
    crpc_test::skip("requires CRPC_HAVE_COROCC (the coroutine form is corocc-only)");
#else
    rpc_pair pair("net-ping-rpc-co");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    crpc::async::executor_options options;
    options.threads = 2;
    options.queue_capacity = 16;
    crpc::async::executor pool(options);

    rpc_coro_job job;
    job.shard = &shard;
    job.pool = &pool;
    job.client = pair.client.get();
    CRPC_CHECK(shard.spawn(&rpc_coro_entry, &job));
    const bool finished = crpc_test::wait_for(
        [&] { return job.done.load(std::memory_order_acquire); }, 10000);

    shard.stop();
    shard.join();

    CRPC_CHECK(finished);
    if (!finished) {
        return;
    }
    CRPC_CHECK(job.reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, job.reply.code);
    CRPC_CHECK(job.reply.rtt >= std::chrono::nanoseconds{0});
    CRPC_CHECK(job.reply.rtt < k_loopback_timeout);
#endif
}

CRPC_TEST(net_ping_rpc_with_flow_control_enabled) {
    rpc_pair pair("net-ping-rpc-flow");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }
    crpc::rpc::flow_control_options flow;
    flow.enabled = true;
    pair.client->set_flow_control(flow);
    const crpc::net::ping_reply reply =
        crpc::net::rpc_ping_sync(*pair.client, k_loopback_timeout);
    CRPC_CHECK(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::ok, reply.code);
    // The public token marker exists precisely so this probe is not fed to the
    // congestion controller as an RTT sample.
    const crpc::rpc::flow_state snapshot = pair.client->flow_snapshot();
    CRPC_CHECK_EQ(crpc::u64{0}, snapshot.probe_samples);
}

CRPC_TEST(net_ping_rpc_zero_timeout_is_immediate) {
    rpc_pair pair("net-ping-rpc-zero");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    const crpc::net::ping_reply reply =
        crpc::net::rpc_ping_sync(*pair.client, std::chrono::nanoseconds{0});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::deadline_exceeded, reply.code);
    CRPC_CHECK_FALSE(reply.error.empty());
    CRPC_CHECK(elapsed < 1s);
}

CRPC_TEST(net_ping_rpc_blackhole_times_out_without_hanging) {
    crpc::rpc::client client(std::make_unique<blackhole_connection>());
    const auto started = std::chrono::steady_clock::now();
    const crpc::net::ping_reply reply =
        crpc::net::rpc_ping_sync(client, k_rpc_blackhole_timeout);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::deadline_exceeded, reply.code);
    CRPC_CHECK_FALSE(reply.error.empty());
    // It really waited for the deadline (not an immediate fabricated error)...
    CRPC_CHECK(elapsed >= 100ms);
    // ...and it returned instead of hanging the caller.
    CRPC_CHECK(elapsed < 3s);
}

CRPC_TEST(net_ping_rpc_closed_client_fails_clearly) {
    rpc_pair pair("net-ping-rpc-closed");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }
    pair.client->close();
    const auto started = std::chrono::steady_clock::now();
    const crpc::net::ping_reply reply =
        crpc::net::rpc_ping_sync(*pair.client, 200ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, reply.code);
    CRPC_CHECK_FALSE(reply.error.empty());
    CRPC_CHECK(elapsed < 1s);
}

CRPC_TEST(net_ping_rpc_negative_timeout_rejected) {
    rpc_pair pair("net-ping-rpc-negative");
    CRPC_CHECK(pair.start());
    if (!pair.client) {
        return;
    }
    const crpc::net::ping_reply reply =
        crpc::net::rpc_ping_sync(*pair.client, -1ms);
    CRPC_CHECK_FALSE(reply.replied);
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, reply.code);
    CRPC_CHECK_FALSE(reply.error.empty());
}

CRPC_TEST_MAIN()
