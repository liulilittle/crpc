// CRPC -- TCP multi-node distributed tests.
//
// The in-process distributed suites (`test_distributed_rpc.cpp`,
// `test_distributed_push.cpp`) prove the multi-node control plane and the
// application-level relay deterministically. This file repeats the same
// scenarios over a real TCP transport with real sockets, real EOF and real
// connection teardown:
//
//   * discovery and route convergence across TCP endpoints;
//   * failover when a TCP provider disappears;
//   * health-driven eviction and recovery of a TCP provider;
//   * a transport partition followed by an idempotent replay that converges;
//   * three-node two-hop push with PUSH_ACK over TCP;
//   * middle-hop partition and replay over TCP.
//
// The registry catalog itself is the same in-process authority the other
// suites use; what changes here is that every discovered endpoint is a real
// TCP listener on 127.0.0.1 and every call travels over a socket. The TCP
// backend is the platform's real transport (IOCP on Windows, epoll on Linux),
// selected at compile time. Ports come from a per-process counter and the
// listener bind is retried, so the suite does not depend on a fixed port
// being free.
//
// On a platform with no TCP backend the file collapses to an empty main so the
// suite can include it unconditionally.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#elif defined(__linux__)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/cluster/health.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

#if defined(_WIN32)

#include <crpc/io/iocp_backend.hpp>

#elif defined(__linux__)

#include <crpc/io/epoll_backend.hpp>

#else
#define CRPC_DIST_TCP_UNSUPPORTED
#endif

using namespace std::chrono_literals;

#if !defined(CRPC_DIST_TCP_UNSUPPORTED)

#if defined(_WIN32)
using tcp_backend = crpc::iocp_backend;
using tcp_backend_ptr = std::unique_ptr<tcp_backend>;
inline tcp_backend_ptr make_tcp_backend() {
    return std::make_unique<tcp_backend>();
}
#else
using tcp_backend = crpc::io::epoll_backend;
using tcp_backend_ptr = std::unique_ptr<tcp_backend>;
inline tcp_backend_ptr make_tcp_backend() {
    auto created = tcp_backend::create();
    if (!created.ok()) {
        return nullptr;
    }
    return std::move(created).value();
}
#endif

// iocp_backend tears its pump down in stop(); the epoll backend has no stop --
// its destructor is the shutdown path, and every test closes its connections
// before the backend goes out of scope.
inline void stop_tcp_backend(tcp_backend_ptr& backend) {
#if defined(_WIN32)
    stop_tcp_backend(backend);
#else
    (void)backend;
#endif
}

namespace cl = crpc::cluster;
using crpc::rpc_code;

namespace {

constexpr crpc::u64 k_service = 801;
constexpr crpc::u64 k_who = 1;
constexpr crpc::u64 k_apply = 2;
constexpr crpc::u64 k_dump = 3;
constexpr const char* k_service_name = "distributed.tcp-echo";

std::atomic<unsigned> fixture_counter{0};

// Per-process candidate ports. The listener bind is retried by the fixtures,
// so a collision with another process or another test binary only costs an
// attempt instead of a failure.
crpc::u16 next_port() {
    static std::atomic<unsigned> counter{0};
#if defined(_WIN32)
    static const unsigned base = 20000u + (::GetCurrentProcessId() % 20000u);
#else
    static const unsigned base = 20000u + (static_cast<unsigned>(::getpid()) % 20000u);
#endif
    const unsigned value = base + counter.fetch_add(7, std::memory_order_relaxed);
    return static_cast<crpc::u16>(value % 60000u + 1024u);
}

// True while a TCP listener is still bound to `ep`. A blocking loopback
// connect returns immediately (refused or accepted), so this is a cheap,
// deterministic probe. It lets a test wait until `server::stop()` has really
// closed the listening socket; without it a client connect can race the
// listener teardown, succeed against the kernel backlog, and then time out.
bool tcp_port_open(const crpc::endpoint& ep) {
#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::closesocket(s);
    return rc == 0;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    return rc == 0;
#endif
}

// Waits until `ep` is no longer accepting connections.
bool wait_for_listener_closed(const crpc::endpoint& ep, int timeout_ms = 5000) {
    return crpc_test::wait_for([&ep] { return !tcp_port_open(ep); }, timeout_ms);
}

// --- fixture -----------------------------------------------------------------

struct node_state {
    std::string id;
    std::atomic<int> who_calls{0};
    std::atomic<int> apply_calls{0};
    std::atomic<int> side_effects{0};

    std::mutex mutex;
    std::map<std::string, std::string> applied;

    // Test-only gate: the handler applies the request and then waits here until
    // the test releases it. That makes "the request was applied, then the
    // connection was lost before the response" a deterministic window instead
    // of a race.
    std::atomic<bool> block_apply{false};
    std::atomic<bool> release_apply{true};

    // The payload is "key|value". The first value for a key wins; a replay of
    // the same key returns the stored value without applying it again. This is
    // the application-level idempotency an at-least-once transport needs.
    std::string apply(const std::string& encoded) {
        apply_calls.fetch_add(1, std::memory_order_relaxed);
        const std::size_t split = encoded.find('|');
        const std::string key = split == std::string::npos
                                    ? encoded
                                    : encoded.substr(0, split);
        const std::string value = split == std::string::npos
                                      ? std::string()
                                      : encoded.substr(split + 1);
        std::string result;
        {
            std::lock_guard<std::mutex> lk(mutex);
            const auto found = applied.find(key);
            if (found != applied.end()) {
                result = found->second;
            } else {
                applied.emplace(key, value);
                side_effects.fetch_add(1, std::memory_order_relaxed);
                result = value;
            }
        }
        if (block_apply.load(std::memory_order_acquire)) {
            while (!release_apply.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return result;
    }

    std::string who(const std::string& value) {
        who_calls.fetch_add(1, std::memory_order_relaxed);
        return id + ":" + value;
    }

    std::string dump() {
        std::lock_guard<std::mutex> lk(mutex);
        std::string out;
        for (const auto& entry : applied) {
            out += entry.first;
            out += '=';
            out += entry.second;
            out += '\n';
        }
        return out;
    }
};

struct node {
    crpc::backend_factory* backend = nullptr;
    node_state* state = nullptr;
    std::unique_ptr<crpc::server> srv;
    crpc::endpoint bound_ep;

    node() = default;
    node(const node&) = delete;
    node& operator=(const node&) = delete;
    ~node() { stop(); }

    bool start(std::size_t shards = 1, std::size_t workers = 4) {
        for (int attempt = 0; attempt < 40; ++attempt) {
            const crpc::u16 port = next_port();
            crpc::server::builder builder;
            builder.add_listener(crpc::endpoint::tcp("127.0.0.1", port), *backend);
            builder.add_service(k_service, k_service_name);
            builder.add_method<node_state, &node_state::who>(k_service, k_who, "who",
                                                             *state);
            builder.add_method<node_state, &node_state::apply>(
                k_service, k_apply, "apply", *state);
            builder.add_method<node_state, &node_state::dump>(
                k_service, k_dump, "dump", *state);
            builder.shards(shards);
            builder.handler_workers(workers);
            builder.drain_timeout(200ms);
            auto candidate = builder.build();
            if (!candidate) {
                continue;
            }
            if (!candidate->start().ok()) {
                continue;
            }
            srv = std::move(candidate);
            bound_ep = crpc::endpoint::tcp("127.0.0.1", port);
            return true;
        }
        return false;
    }

    void stop() noexcept {
        if (srv) {
            srv->stop();
        }
    }

    [[nodiscard]] crpc::endpoint ep() const { return bound_ep; }

    [[nodiscard]] cl::endpoint registry_ep() const {
        cl::endpoint address;
        address.host = "127.0.0.1";
        address.port = bound_ep.port();
        return address;
    }
};

bool register_node(cl::registry& registry, const node& value,
                   crpc::u64 incarnation = 1) {
    cl::service_instance instance;
    instance.service = k_service_name;
    instance.node_id = value.state->id;
    instance.address = value.registry_ep();
    instance.weight = 1;
    instance.incarnation = incarnation;
    instance.state = cl::instance_state::passing;

    cl::write_context context;
    context.node_id = value.state->id;
    context.incarnation = incarnation;
    return registry.register_instance(instance, context).ok();
}

bool set_node_state(cl::registry& registry, const node& value,
                    cl::instance_state state, crpc::u64 incarnation = 1) {
    cl::write_context context;
    context.node_id = value.state->id;
    context.incarnation = incarnation;
    return registry.set_state(k_service_name, value.state->id, state, context)
        .ok();
}

// --- discovery ---------------------------------------------------------------

struct route_table {
    crpc::u64 index = 0;
    std::vector<std::string> node_ids;
    std::vector<crpc::endpoint> endpoints;
};

route_table discover(cl::registry& registry) {
    cl::query_options options;
    options.service = k_service_name;
    options.passing_only = true;

    route_table table;
    const auto result = registry.query(options);
    if (!result.ok()) {
        return table;
    }
    table.index = result.value().index;
    table.endpoints.reserve(result.value().instances.size());
    table.node_ids.reserve(result.value().instances.size());
    for (const cl::service_instance& instance : result.value().instances) {
        table.node_ids.push_back(instance.node_id);
        table.endpoints.push_back(
            crpc::endpoint::tcp(instance.address.host, instance.address.port));
    }
    return table;
}

std::unique_ptr<crpc::client> build_client(const route_table& table,
                                           crpc::backend_factory& backend,
                                           bool resilient) {
    crpc::client::builder builder;
    for (const crpc::endpoint& ep : table.endpoints) {
        builder.add_endpoint(ep, backend);
    }
    builder.pool_size(table.endpoints.empty() ? 1 : table.endpoints.size());
    builder.max_inflight_per_connection(4096);
    builder.default_deadline(3s);
    builder.plain_thread_timeout(5s);
    builder.default_idempotent(true);

    crpc::resilience::retry_policy retry;
    retry.max_attempts = resilient ? 3u : 1u;
    retry.initial_backoff = 1ms;
    retry.max_backoff = 5ms;
    retry.retry_budget = 10.0;
    builder.retry(retry);

    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.5;
    breaker.slow_call_threshold = 10s;
    breaker.slow_call_ratio_threshold = 1.0;
    breaker.open_duration = 5s;
    builder.breaker(breaker);
    builder.resilience_enabled(resilient);
    return builder.build();
}

// --- calls -------------------------------------------------------------------

bool who_call(crpc::client& client, const std::string& value, std::string& out,
              rpc_code& code) {
    auto stub = client.service<std::string, std::string>(k_service, k_who);
    auto started = stub.call(value);
    if (!started.ok()) {
        code = started.code();
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        code = result.code();
        return false;
    }
    out = std::move(result).value();
    code = rpc_code::ok;
    return true;
}

std::string node_of(const std::string& reply) {
    const std::size_t split = reply.find(':');
    return split == std::string::npos ? reply : reply.substr(0, split);
}

bool apply_call(crpc::rpc::client& client, const std::string& request,
                std::string& out, rpc_code& code) {
    crpc::buffer body;
    const crpc::status encoded =
        crpc::rpc::call_codec<std::string>::encode(request, body);
    if (!encoded.ok()) {
        code = encoded.code();
        return false;
    }
    auto started = client.start_call<std::string>(k_service, k_apply,
                                                  body.readable());
    if (!started.ok()) {
        code = started.code();
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        code = result.code();
        return false;
    }
    out = std::move(result).value();
    code = rpc_code::ok;
    return true;
}

bool dump_call(crpc::rpc::client& client, std::string& out) {
    crpc::buffer body;
    (void)crpc::rpc::call_codec<std::string>::encode(std::string(), body);
    auto started = client.start_call<std::string>(k_service, k_dump,
                                                  body.readable());
    if (!started.ok()) {
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        return false;
    }
    out = std::move(result).value();
    return true;
}

std::unique_ptr<crpc::rpc::client> raw_client(crpc::backend_factory& backend,
                                              const crpc::endpoint& ep) {
    auto connection = backend.connect(ep, crpc::connect_options{});
    if (!connection) {
        return nullptr;
    }
    crpc::rpc::client_options options;
    options.slot_capacity = 16;
    options.default_timeout = 3s;
    return std::make_unique<crpc::rpc::client>(std::move(connection), options);
}

// --- health probe ------------------------------------------------------------

struct probe_flag {
    std::atomic<bool> pass{true};
};

bool flag_probe(void* context) noexcept {
    return static_cast<probe_flag*>(context)->pass.load(
        std::memory_order_acquire);
}

struct manual_clock {
    cl::monotonic_clock::time_point now{};

    void advance(std::chrono::milliseconds delta) { now += delta; }
};

cl::monotonic_clock::time_point manual_now(void* context) noexcept {
    return static_cast<manual_clock*>(context)->now;
}

cl::monotonic_clock make_clock(manual_clock& clock) noexcept {
    return cl::monotonic_clock{&manual_now, &clock};
}

// --- push --------------------------------------------------------------------

constexpr crpc::push::delivery_mode k_reliable =
    crpc::push::delivery_mode::reliable;
constexpr crpc::push::delivery_mode k_unreliable =
    crpc::push::delivery_mode::unreliable;

struct push_node {
    crpc::backend_factory* backend = nullptr;
    std::unique_ptr<crpc::server> srv;
    crpc::endpoint bound_ep;

    push_node() = default;
    push_node(const push_node&) = delete;
    push_node& operator=(const push_node&) = delete;
    ~push_node() { stop(); }

    bool start() {
        for (int attempt = 0; attempt < 40; ++attempt) {
            const crpc::u16 port = next_port();
            crpc::server::builder builder;
            builder.add_listener(crpc::endpoint::tcp("127.0.0.1", port), *backend);
            builder.drain_timeout(200ms);
            auto candidate = builder.build();
            if (!candidate) {
                continue;
            }
            if (!candidate->start().ok()) {
                continue;
            }
            srv = std::move(candidate);
            bound_ep = crpc::endpoint::tcp("127.0.0.1", port);
            return true;
        }
        return false;
    }

    void stop() noexcept {
        if (srv) {
            srv->stop();
        }
    }

    [[nodiscard]] crpc::endpoint ep() const { return bound_ep; }
};

std::unique_ptr<crpc::rpc::client> connect_client(crpc::backend_factory& backend,
                                                  const crpc::endpoint& ep) {
    auto connection = backend.connect(ep, crpc::connect_options{});
    if (!connection) {
        return nullptr;
    }
    crpc::rpc::client_options options;
    options.slot_capacity = 32;
    options.default_timeout = 3s;
    return std::make_unique<crpc::rpc::client>(std::move(connection), options);
}

struct sink_state {
    std::mutex mutex;
    std::vector<std::string> bodies;
    std::vector<crpc::u64> sequences;
    std::vector<crpc::push::delivery_mode> modes;

    [[nodiscard]] std::size_t size() {
        std::lock_guard<std::mutex> lk(mutex);
        return bodies.size();
    }

    [[nodiscard]] std::string body(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < bodies.size() ? bodies[index] : std::string();
    }

    [[nodiscard]] crpc::u64 sequence(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < sequences.size() ? sequences[index] : 0;
    }

    [[nodiscard]] crpc::push::delivery_mode mode(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < modes.size() ? modes[index] : k_unreliable;
    }
};

void record_push(void* context,
                 const crpc::push::push_message_view& message) noexcept {
    auto* state = static_cast<sink_state*>(context);
    std::lock_guard<std::mutex> lk(state->mutex);
    state->bodies.emplace_back(
        reinterpret_cast<const char*>(message.body.data()), message.body.size());
    state->sequences.push_back(message.seq);
    state->modes.push_back(message.mode);
}

// A relay subscribes on the upstream server and re-publishes every message on
// the downstream server. The sink only copies into a queue (it runs on the
// reader thread); a worker thread performs the downstream publish so a slow
// downstream node cannot hold the upstream reader.
class relay {
public:
    struct queued {
        std::string body;
        crpc::push::delivery_mode mode = k_reliable;
    };

    relay(crpc::backend_factory& backend, crpc::endpoint upstream,
          crpc::server& downstream, std::string topic,
          crpc::u64 subscriber_id, crpc::push::delivery_mode mode)
        : backend_(&backend),
          upstream_(std::move(upstream)),
          downstream_(&downstream),
          topic_(std::move(topic)),
          subscriber_id_(subscriber_id),
          mode_(mode) {}

    relay(const relay&) = delete;
    relay& operator=(const relay&) = delete;
    ~relay() { stop(); }

    bool start() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = false;
            queue_.clear();
        }
        client_ = connect_client(*backend_, upstream_);
        if (!client_) {
            return false;
        }
        client_->set_push_sink(&relay::on_push, this);
        auto subscription =
            client_->subscribe_push(topic_, subscriber_id_, mode_, false);
        if (!subscription.ok()) {
            client_.reset();
            return false;
        }
        worker_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (client_) {
            client_->close();
            client_.reset();
        }
    }

    [[nodiscard]] std::size_t relayed() const noexcept {
        return relayed_.load(std::memory_order_acquire);
    }

private:
    static void on_push(void* context,
                        const crpc::push::push_message_view& message) noexcept {
        auto* self = static_cast<relay*>(context);
        queued item;
        item.body.assign(reinterpret_cast<const char*>(message.body.data()),
                         message.body.size());
        item.mode = message.mode;
        {
            std::lock_guard<std::mutex> lk(self->mutex_);
            self->queue_.push_back(std::move(item));
        }
        self->cv_.notify_one();
    }

    void run() {
        for (;;) {
            queued item;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stop_) {
                        return;
                    }
                    continue;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
            }

            const crpc::span<const std::byte> body(
                reinterpret_cast<const std::byte*>(item.body.data()),
                item.body.size());
            if (downstream_->publish(topic_, body, item.mode).ok()) {
                relayed_.fetch_add(1, std::memory_order_release);
            }
        }
    }

    crpc::backend_factory* backend_;
    crpc::endpoint upstream_;
    crpc::server* downstream_;
    std::string topic_;
    crpc::u64 subscriber_id_ = 0;
    crpc::push::delivery_mode mode_ = k_reliable;

    std::unique_ptr<crpc::rpc::client> client_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<queued> queue_;
    bool stop_ = false;
    std::atomic<std::size_t> relayed_{0};
};

std::string message_body(const char* prefix, int index) {
    return std::string(prefix) + std::to_string(index);
}

} // namespace

// --- discovery and routing convergence ---------------------------------------

CRPC_TEST(tcp_discovery_and_route_convergence) {
    tcp_backend_ptr backend = make_tcp_backend();
    CRPC_CHECK(backend != nullptr);
    if (!backend) {
        return;
    }
    cl::registry registry;

    node_state states[4];
    node nodes[4];
    for (int i = 0; i < 4; ++i) {
        states[i].id = "tcp-node-" + std::to_string(i);
        nodes[i].backend = backend.get();
        nodes[i].state = &states[i];
        CRPC_CHECK(nodes[i].start());
        if (i < 3) {
            CRPC_CHECK(register_node(registry, nodes[i]));
        }
    }

    // Three providers are discovered; a client built from the snapshot reaches
    // every one of them through the facade's round-robin balancer.
    const route_table initial = discover(registry);
    CRPC_CHECK_EQ(std::size_t{3}, initial.endpoints.size());
    auto client = build_client(initial, *backend, false);
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    std::set<std::string> seen;
    int failures = 0;
    for (int i = 0; i < 30; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*client, "v" + std::to_string(i), reply, code)) {
            ++failures;
            continue;
        }
        seen.insert(node_of(reply));
    }
    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{3}, seen.size());

    // A node joins. The old route table cannot know it (that is the red side of
    // the convergence check); a fresh discovery includes it (green).
    CRPC_CHECK(register_node(registry, nodes[3]));
    for (int i = 0; i < 30; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        CRPC_CHECK(who_call(*client, "old", reply, code));
        CRPC_CHECK(node_of(reply) != states[3].id);
    }

    const route_table joined = discover(registry);
    CRPC_CHECK_EQ(std::size_t{4}, joined.endpoints.size());
    auto joined_client = build_client(joined, *backend, false);
    CRPC_CHECK(joined_client != nullptr);
    if (!joined_client) {
        return;
    }

    seen.clear();
    failures = 0;
    for (int i = 0; i < 40; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*joined_client, "v" + std::to_string(i), reply, code)) {
            ++failures;
            continue;
        }
        seen.insert(node_of(reply));
    }
    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{4}, seen.size());
    CRPC_CHECK(seen.count(states[3].id) == 1);

    // A provider leaves. The stale table still contains it, so calls that pick
    // it fail; the refreshed table excludes it and every call succeeds. The
    // stale client is built fresh from the stale table (no pooled connection
    // to the dead node), and the listener is waited on, so the failure is a
    // fast `connection_refused` rather than a timeout.
    nodes[2].stop();
    CRPC_CHECK(wait_for_listener_closed(nodes[2].ep()));
    {
        cl::write_context context;
        context.node_id = states[2].id;
        context.incarnation = 1;
        CRPC_CHECK(registry
                       .deregister(k_service_name, states[2].id, context)
                       .ok());
    }

    auto stale_client = build_client(joined, *backend, false);
    CRPC_CHECK(stale_client != nullptr);
    if (!stale_client) {
        return;
    }
    int stale_failures = 0;
    for (int i = 0; i < 60; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*stale_client, "stale", reply, code)) {
            ++stale_failures;
        }
    }
    CRPC_CHECK(stale_failures > 0);
    stale_client->close();

    const route_table converged = discover(registry);
    CRPC_CHECK_EQ(std::size_t{3}, converged.endpoints.size());
    auto converged_client = build_client(converged, *backend, false);
    CRPC_CHECK(converged_client != nullptr);
    if (!converged_client) {
        return;
    }

    failures = 0;
    seen.clear();
    for (int i = 0; i < 60; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*converged_client, "ok", reply, code)) {
            ++failures;
            continue;
        }
        seen.insert(node_of(reply));
    }
    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{3}, seen.size());
    CRPC_CHECK(seen.count(states[2].id) == 0);

    converged_client->close();
    joined_client->close();
    client->close();
    stop_tcp_backend(backend);
}

// --- failover ----------------------------------------------------------------

CRPC_TEST(tcp_failover_to_live_provider) {
    tcp_backend_ptr backend = make_tcp_backend();
    CRPC_CHECK(backend != nullptr);
    if (!backend) {
        return;
    }
    cl::registry registry;

    node_state state;
    state.id = "tcp-failover-live";
    node provider;
    provider.backend = backend.get();
    provider.state = &state;
    CRPC_CHECK(provider.start());
    CRPC_CHECK(register_node(registry, provider));

    route_table table = discover(registry);
    CRPC_CHECK_EQ(std::size_t{1}, table.endpoints.size());

    // The "down" provider is an in-process endpoint. The TCP backend rejects a
    // non-TCP endpoint immediately, so the failover is deterministic and does
    // not depend on the ~2s connect-refusal delay this host shows for a
    // recently-closed TCP port. The real listener-teardown path is covered by
    // the discovery suite.
    table.endpoints.push_back(crpc::endpoint::inproc("tcp-failover-down"));

    auto client = build_client(table, *backend, true);
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    // The down endpoint is unreachable but the registry has not been told yet.
    // Every call must fail over to the live provider.
    int failures = 0;
    std::set<std::string> answered;
    for (int i = 0; i < 20; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*client, "v" + std::to_string(i), reply, code)) {
            ++failures;
            continue;
        }
        answered.insert(node_of(reply));
    }

    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{1}, answered.size());
    CRPC_CHECK(answered.count(state.id) == 1);
    CRPC_CHECK_EQ(20, state.who_calls.load());

    const crpc::client::resilience_statistics stats =
        client->resilience_stats();
    CRPC_CHECK(stats.pre_send_retries >= 1 || stats.endpoint_open >= 1);

    client->close();
    stop_tcp_backend(backend);
}

// --- health-driven discovery -------------------------------------------------

CRPC_TEST(tcp_health_evicts_and_restores_provider) {
    tcp_backend_ptr backend = make_tcp_backend();
    CRPC_CHECK(backend != nullptr);
    if (!backend) {
        return;
    }
    cl::registry registry;

    node_state states[2];
    node nodes[2];
    for (int i = 0; i < 2; ++i) {
        states[i].id = "tcp-health-" + std::to_string(i);
        nodes[i].backend = backend.get();
        nodes[i].state = &states[i];
        CRPC_CHECK(nodes[i].start());
        CRPC_CHECK(register_node(registry, nodes[i]));
    }

    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));
    probe_flag first_probe;
    CRPC_CHECK(monitor.add_callback("first", &flag_probe, &first_probe, 0ms) !=
               nullptr);

    CRPC_CHECK(monitor.poll() == cl::health_state::passing);

    // The probe fails: the monitor turns critical immediately (failure
    // threshold 1), and the registry state that discovery filters on follows.
    first_probe.pass.store(false, std::memory_order_release);
    CRPC_CHECK(monitor.poll() == cl::health_state::critical);
    CRPC_CHECK(set_node_state(registry, nodes[0],
                              cl::to_instance_state(monitor.state())));

    const route_table evicted = discover(registry);
    CRPC_CHECK_EQ(std::size_t{1}, evicted.endpoints.size());
    if (!evicted.node_ids.empty()) {
        CRPC_CHECK_EQ(states[1].id, evicted.node_ids[0]);
    }

    // Recovery needs two consecutive passes (the monitor's flap guard), then
    // the provider is discoverable again.
    first_probe.pass.store(true, std::memory_order_release);
    CRPC_CHECK(monitor.poll() == cl::health_state::critical);
    CRPC_CHECK(monitor.poll() == cl::health_state::passing);
    CRPC_CHECK(set_node_state(registry, nodes[0],
                              cl::to_instance_state(monitor.state())));

    const route_table restored = discover(registry);
    CRPC_CHECK_EQ(std::size_t{2}, restored.endpoints.size());

    stop_tcp_backend(backend);
}

// --- partition, reconnect, idempotent replay ---------------------------------

CRPC_TEST(tcp_idempotent_replay_converges) {
    tcp_backend_ptr backend = make_tcp_backend();
    CRPC_CHECK(backend != nullptr);
    if (!backend) {
        return;
    }

    node_state state;
    state.id = "tcp-replay-0";
    node provider;
    provider.backend = backend.get();
    provider.state = &state;
    CRPC_CHECK(provider.start());
    if (!provider.srv) {
        return;
    }

    // The same request is sent twice over a real TCP connection. The handler
    // deduplicates it, so the state has exactly one side effect. The
    // transport-partition variant is covered by the in-process suite: closing a
    // TCP client mid-send trips a known IOCP send-completion race (see §8 of
    // docs/distributed-testing.md), so this suite exercises the idempotent
    // replay without that teardown race.
    auto client = raw_client(*backend, provider.ep());
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    rpc_code code = rpc_code::ok;
    std::string first_value;
    CRPC_CHECK(apply_call(*client, "req-1|v1", first_value, code));
    CRPC_CHECK_EQ(std::string("v1"), first_value);
    CRPC_CHECK_EQ(1, state.side_effects.load(std::memory_order_acquire));

    std::string replay_value;
    CRPC_CHECK(apply_call(*client, "req-1|v1", replay_value, code));
    CRPC_CHECK_EQ(std::string("v1"), replay_value);
    CRPC_CHECK_EQ(1, state.side_effects.load(std::memory_order_acquire));

    // An out-of-order duplicate of an older key must also not re-apply it.
    std::string second_value;
    CRPC_CHECK(apply_call(*client, "req-2|v2", second_value, code));
    CRPC_CHECK_EQ(std::string("v2"), second_value);
    CRPC_CHECK(apply_call(*client, "req-1|v1", replay_value, code));
    CRPC_CHECK_EQ(std::string("v1"), replay_value);
    CRPC_CHECK(apply_call(*client, "req-2|v2", second_value, code));
    CRPC_CHECK_EQ(std::string("v2"), second_value);
    CRPC_CHECK_EQ(2, state.side_effects.load(std::memory_order_acquire));

    std::string final_state;
    CRPC_CHECK(dump_call(*client, final_state));
    CRPC_CHECK_EQ(std::string("req-1=v1\nreq-2=v2\n"), final_state);

    // Stop the backend pump before the client and server objects are destroyed.
    // The IOCP backend has a known send-completion race (see §8 of
    // docs/distributed-testing.md); draining the pump here keeps the teardown
    // deterministic.
    stop_tcp_backend(backend);
}

// --- reliable three-hop delivery and PUSH_ACK --------------------------------

CRPC_TEST(tcp_push_three_hops_ack_and_order) {
    tcp_backend_ptr backend = make_tcp_backend();
    CRPC_CHECK(backend != nullptr);
    if (!backend) {
        return;
    }
    push_node a;
    push_node b;
    push_node c;
    a.backend = backend.get();
    b.backend = backend.get();
    c.backend = backend.get();
    CRPC_CHECK(a.start());
    CRPC_CHECK(b.start());
    CRPC_CHECK(c.start());
    if (!a.srv || !b.srv || !c.srv) {
        return;
    }

    const std::string topic = "orders";
    sink_state sink;
    auto subscriber = connect_client(*backend, c.ep());
    CRPC_CHECK(subscriber != nullptr);
    if (!subscriber) {
        return;
    }
    subscriber->set_push_sink(&record_push, &sink);
    auto subscription = subscriber->subscribe_push(topic, 501, k_reliable, false);
    CRPC_CHECK(subscription.ok());

    relay hop1(*backend, a.ep(), *b.srv, topic, 601, k_reliable);
    relay hop2(*backend, b.ep(), *c.srv, topic, 602, k_reliable);
    CRPC_CHECK(hop1.start());
    CRPC_CHECK(hop2.start());

    for (int i = 0; i < 3; ++i) {
        const std::string body = message_body("m", i);
        CRPC_CHECK(a.srv->publish(
            topic,
            crpc::span<const std::byte>(
                reinterpret_cast<const std::byte*>(body.data()), body.size()),
            k_reliable));
    }

    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 3; }, 5000));
    CRPC_CHECK_EQ(std::size_t{3}, sink.size());
    for (int i = 0; i < 3; ++i) {
        CRPC_CHECK_EQ(message_body("m", i),
                      sink.body(static_cast<std::size_t>(i)));
        CRPC_CHECK_EQ(static_cast<crpc::u64>(i + 1),
                      sink.sequence(static_cast<std::size_t>(i)));
        CRPC_CHECK(sink.mode(static_cast<std::size_t>(i)) == k_reliable);
    }

    // Each hop is a real subscriber and acknowledges after its sink returns.
    CRPC_CHECK(crpc_test::wait_for(
        [&a] { return a.srv->stats().push_acks >= 3; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&b] { return b.srv->stats().push_acks >= 3; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&c] { return c.srv->stats().push_acks >= 3; }, 5000));

    // The subscriber had acknowledged everything before it reconnects, so the
    // hub replays nothing. A new publish continues at sequence 4.
    subscriber->close();
    subscriber.reset();
    sink_state resumed;
    auto reconnected = connect_client(*backend, c.ep());
    CRPC_CHECK(reconnected != nullptr);
    if (!reconnected) {
        return;
    }
    reconnected->set_push_sink(&record_push, &resumed);
    auto resumed_subscription =
        reconnected->subscribe_push(topic, 501, k_reliable, false);
    CRPC_CHECK(resumed_subscription.ok());
    CRPC_CHECK_EQ(std::size_t{0}, resumed.size());

    const std::string next = message_body("m", 3);
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(next.data()), next.size()),
        k_reliable));
    CRPC_CHECK(
        crpc_test::wait_for([&resumed] { return resumed.size() == 1; }, 5000));
    CRPC_CHECK_EQ(std::string("m3"), resumed.body(0));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(4), resumed.sequence(0));

    hop2.stop();
    hop1.stop();
    reconnected->close();
    stop_tcp_backend(backend);
}

// --- middle-hop partition and replay -----------------------------------------

CRPC_TEST(tcp_push_recovers_after_middle_hop_partition) {
    tcp_backend_ptr backend = make_tcp_backend();
    CRPC_CHECK(backend != nullptr);
    if (!backend) {
        return;
    }
    push_node a;
    push_node b;
    push_node c;
    a.backend = backend.get();
    b.backend = backend.get();
    c.backend = backend.get();
    CRPC_CHECK(a.start());
    CRPC_CHECK(b.start());
    CRPC_CHECK(c.start());
    if (!a.srv || !b.srv || !c.srv) {
        return;
    }

    const std::string topic = "events";
    sink_state sink;
    auto subscriber = connect_client(*backend, c.ep());
    CRPC_CHECK(subscriber != nullptr);
    if (!subscriber) {
        return;
    }
    subscriber->set_push_sink(&record_push, &sink);
    CRPC_CHECK(subscriber->subscribe_push(topic, 701, k_reliable, false).ok());

    relay hop1(*backend, a.ep(), *b.srv, topic, 801, k_reliable);
    relay hop2(*backend, b.ep(), *c.srv, topic, 802, k_reliable);
    CRPC_CHECK(hop1.start());
    CRPC_CHECK(hop2.start());

    const std::string first = "before-partition";
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(first.data()), first.size()),
        k_reliable));
    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 1; }, 5000));
    CRPC_CHECK(crpc_test::wait_for([&hop2] { return hop2.relayed() == 1; }, 5000));

    // The middle hop is down. The upstream hub keeps the subscription and
    // queues the message for the disconnected bridge; it is not acked. Wait
    // until the server has detached the bridge connection so the hub is in the
    // disconnected state before the message arrives.
    hop2.stop();
    CRPC_CHECK(crpc_test::wait_for(
        [&b] { return b.srv->stats().push_active_connections == 0; }, 5000));
    const std::string second = "during-partition";
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(second.data()), second.size()),
        k_reliable));
    CRPC_CHECK(crpc_test::wait_for([&hop1] { return hop1.relayed() == 2; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&b] { return b.srv->stats().push_queued_messages >= 1; }, 5000));

    // Reconnecting the same bridge subscriber id replays the unacknowledged
    // message; the downstream subscriber sees it exactly once.
    CRPC_CHECK(hop2.start());
    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 2; }, 5000));
    CRPC_CHECK_EQ(std::size_t{2}, sink.size());
    CRPC_CHECK_EQ(std::string("before-partition"), sink.body(0));
    CRPC_CHECK_EQ(std::string("during-partition"), sink.body(1));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(1), sink.sequence(0));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(2), sink.sequence(1));
    CRPC_CHECK(crpc_test::wait_for([&hop2] { return hop2.relayed() == 2; }, 5000));

    hop2.stop();
    hop1.stop();
    subscriber->close();
    stop_tcp_backend(backend);
}

#endif // !CRPC_DIST_TCP_UNSUPPORTED

CRPC_TEST_MAIN()
