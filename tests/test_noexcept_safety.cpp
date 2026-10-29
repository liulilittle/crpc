// CRPC -- noexcept allocation-safety regression.
//
// Several `noexcept` paths allocate (a vector snapshot, a long message string,
// a metadata entry). An allocation failure there used to escape the `noexcept`
// function and call `std::terminate`. Each case below arms a one-shot
// allocation failure with a test-local global `operator new` override, runs the
// path, and asserts that the process survived and that the injection actually
// fired (`g_injected_alloc_failures`). The counter matters: an arm that never
// reached the target allocation would otherwise pass as a false green.
//
// The injection threshold is `> 16`. The Debug CRT allocates a 16-byte
// container proxy even for an empty `std::string`, and the `status`/`rpc_error`
// constructors are (implicitly) `noexcept`, so a 16-byte failure would terminate
// inside the constructor before any handler can run. Every allocation targeted
// here (a vector copy of three or more shared_ptrs, a message longer than SSO)
// is larger than 16 bytes.

#include <crpc/buffer.hpp>
#include <crpc/client_facade.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>
#include <crpc/span.hpp>

#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

std::atomic<int> g_injected_alloc_failures{0};
std::atomic<int> g_fail_alloc_budget{0};
thread_local bool g_fail_next_alloc = false;

} // namespace

void* operator new(std::size_t size) {
    if (size > 16) {
        if (g_fail_next_alloc) {
            g_fail_next_alloc = false;
            g_injected_alloc_failures.fetch_add(1, std::memory_order_relaxed);
            throw std::bad_alloc();
        }
        int budget = g_fail_alloc_budget.load(std::memory_order_acquire);
        while (budget > 0) {
            if (g_fail_alloc_budget.compare_exchange_weak(budget, budget - 1,
                                                          std::memory_order_acq_rel)) {
                g_injected_alloc_failures.fetch_add(1, std::memory_order_relaxed);
                throw std::bad_alloc();
            }
        }
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

constexpr crpc::u64 kService = 100;
constexpr crpc::u64 kMethod = 1;

// A connection that accepts writes and never completes a read: calls stay in
// flight so the deadline/close paths have live state to walk.
struct stalled_source final : crpc::io::source {
    std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct stalled_sink final : crpc::io::sink {
    void push(crpc::span<const std::byte>, crpc::io::completion*) noexcept override {}
};

struct stalled_connection final : crpc::connection {
    stalled_source source_;
    stalled_sink sink_;
    std::atomic<bool> open{true};

    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept override {
        return open.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("noexcept-stalled");
    }
};

struct stalled_backend final : crpc::backend_factory {
    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return std::make_unique<stalled_connection>();
    }
    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                           const crpc::listen_options&) override {
        return nullptr;
    }
};

crpc::status noop_handler(void*, const crpc::call_envelope&, crpc::span<const std::byte>,
                          crpc::buffer&, crpc::rpc::call_context&) {
    return crpc::status{};
}

// A server with one in-flight call per pooled connection, used by the GOAWAY and
// dispatch cases.
struct server_under_test {
    crpc::inproc_backend backend;
    std::unique_ptr<crpc::server> srv;
    ~server_under_test() {
        if (srv) {
            srv->stop();
        }
    }
};

} // namespace

// --- decode_response ---------------------------------------------------------

CRPC_TEST(noexcept_decode_response_survives_allocation_failure) {
    // A message longer than SSO forces `decoded.message.assign(...)` to
    // allocate, which is the allocation the fix guards.
    const std::string long_message = "this message is deliberately longer than SSO";
    crpc::rpc_error err{crpc::rpc_code::not_found, long_message};
    crpc::buffer out;
    CRPC_CHECK(crpc::rpc::encode_response(crpc::rpc_code::not_found, err,
                                          crpc::span<const std::byte>(), out)
                   .ok());

    crpc::rpc_code code = crpc::rpc_code::ok;
    crpc::rpc_error decoded;
    crpc::span<const std::byte> body;
    g_fail_next_alloc = true;
    const crpc::status st = crpc::rpc::decode_response(out.readable(), code, decoded, body);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 1);
}

// --- dispatcher::dispatch ----------------------------------------------------

CRPC_TEST(noexcept_dispatch_survives_allocation_failure) {
    crpc::rpc::service_registry registry;
    CRPC_CHECK(registry.register_service(kService, "svc").ok());
    crpc::rpc::dispatcher dispatcher(registry);

    // A truncated envelope makes decode_call_envelope fail, so dispatch builds
    // its own "request decode failed" message, which is longer than SSO.
    crpc::frame request;
    request.header.type = crpc::frame_type::request;
    request.header.stream_id = 1;
    const std::array<std::byte, 2> truncated{std::byte{0x01}, std::byte{0x02}};
    request.payload = crpc::span<const std::byte>(truncated.data(), truncated.size());

    crpc::buffer out;
    bool has_response = false;
    g_fail_next_alloc = true;
    const crpc::status st =
        dispatcher.dispatch(request, out, has_response, std::chrono::steady_clock::now());
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 1);
}

// --- server::send_goaway -----------------------------------------------------

CRPC_TEST(noexcept_send_goaway_survives_allocation_failure) {
    server_under_test f;
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("noexcept-goaway"), f.backend);
    sb.add_service(kService, "svc");
    sb.add_raw_method(kService, kMethod, "noop", &noop_handler, nullptr);
    f.srv = sb.build();
    CRPC_CHECK(f.srv != nullptr);
    CRPC_CHECK(f.srv->start().ok());
    if (!f.srv) {
        return;
    }

    // Three separate clients open three server-side connections, so the GOAWAY
    // snapshot copies 24 bytes (three shared_ptrs) and the injection reaches it.
    std::vector<std::unique_ptr<crpc::client>> clients;
    for (int i = 0; i < 3; ++i) {
        crpc::client::builder cb;
        cb.add_endpoint(crpc::endpoint::inproc("noexcept-goaway"), f.backend);
        cb.pool_size(4);
        auto cli = cb.build();
        CRPC_CHECK(cli != nullptr);
        if (!cli) {
            continue;
        }
        auto stub = cli->service<int, int>(kService, kMethod);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        clients.push_back(std::move(cli));
    }

    g_fail_next_alloc = true;
    f.srv->stop();
    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 1);

    clients.clear();
}

// --- client::close -----------------------------------------------------------

CRPC_TEST(noexcept_client_close_survives_allocation_failure) {
    stalled_backend backend;
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("noexcept-close"), backend);
    cb.pool_size(4);
    cb.default_deadline(5000ms);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        return;
    }

    auto stub = cli->service<int, int>(kService, kMethod);
    auto first = stub.call(1);
    CRPC_CHECK(first.ok());
    auto held = std::make_unique<crpc::client::call<int>>(std::move(first).value());
    auto second = stub.call(2);
    CRPC_CHECK(second.ok());
    auto held_second = std::make_unique<crpc::client::call<int>>(std::move(second).value());

    g_fail_next_alloc = true;
    cli->close();
    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 1);

    held.reset();
    held_second.reset();
    cli.reset();
}

// --- deadline_tick / evict_idle ----------------------------------------------

CRPC_TEST(noexcept_deadline_tick_survives_allocation_failure) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("noexcept-tick"), backend);
    cb.pool_size(4);
    cb.default_deadline(50ms);
    cb.on_shard(shard);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(kService, kMethod);
    auto first = stub.call(1);
    CRPC_CHECK(first.ok());
    auto held = std::make_unique<crpc::client::call<int>>(std::move(first).value());

    // Arm a cross-thread failure and let the shard timer fire. The timer thread
    // snapshots the pool, so its next >16-byte allocation is the target.
    g_fail_alloc_budget.store(1, std::memory_order_release);
    const bool fired = crpc_test::wait_for(
        [] { return g_injected_alloc_failures.load(std::memory_order_acquire) >= 1; }, 5000);
    g_fail_alloc_budget.store(0, std::memory_order_release);
    CRPC_CHECK(fired);

    held.reset();
    cli.reset();
    shard.stop();
    shard.join();
}

// --- rpc::client::poll / fail_all --------------------------------------------

namespace {

std::atomic<std::int64_t> g_clock_ns{0};

std::chrono::steady_clock::time_point test_clock(void*) noexcept {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::nanoseconds{g_clock_ns.load(std::memory_order_acquire)};
}

} // namespace

CRPC_TEST(noexcept_rpc_client_poll_and_close_survive_allocation_failure) {
    crpc::rpc::client_options co;
    co.slot_capacity = 8;
    co.clock = &test_clock;
    crpc::rpc::client cli(std::make_unique<stalled_connection>(), co);

    g_clock_ns.store(0, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::time_point{} + 1000ms;
    auto first = cli.start_call<int>(kService, kMethod, crpc::span<const std::byte>(),
                                     crpc::rpc::with_deadline(crpc::rpc::call_context{}, deadline));
    CRPC_CHECK(first.ok());
    auto second = cli.start_call<int>(kService, kMethod, crpc::span<const std::byte>(),
                                      crpc::rpc::with_deadline(crpc::rpc::call_context{}, deadline));
    CRPC_CHECK(second.ok());

    // Advance past both deadlines, then poll with an injection armed.
    g_clock_ns.store(2000, std::memory_order_release);
    g_fail_next_alloc = true;
    cli.poll();
    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 1);

    // `close()` reaches `fail_all`, whose slot collection also allocates.
    g_fail_next_alloc = true;
    cli.close();
    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 2);
}

CRPC_TEST_MAIN()
