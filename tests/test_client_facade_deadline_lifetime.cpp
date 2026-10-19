// CRPC -- facade deadline-callback lifetime regression.
//
// `client::deadline_thunk` runs on the shard thread and calls `tick_poll()`,
// which polls every pooled rpc::client. A user completion callback may run
// there (the low-level call exposes `then`). If that callback destroys the
// facade client, the timer callback must not touch `this` afterwards and
// tick_poll() must keep its implementation alive. This test destroys the
// client from exactly that callback.
//
// The facade does not expose the low-level call publicly. `call<Resp>` has a
// single data member (`state_`, a shared_ptr) and is standard-layout, so the
// object address is the address of that member; this translation unit uses
// that to install a user callback on the inner call. Access specifiers do not
// affect layout.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/client_facade.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/load_balancer.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/runtime.hpp>
#include <crpc/span.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

struct stalled_source final : crpc::io::source {
    std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct stalled_sink final : crpc::io::sink {
    // Accept and never complete: the call stays in flight with its deadline
    // timer armed until the timer fires.
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
        return crpc::endpoint::inproc("stalled");
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

// `client::call<Resp>` is standard-layout with exactly one data member
// (`state_`), so its address is the address of that member.
template <typename Resp>
std::shared_ptr<crpc::client_detail::call_state<Resp>>& call_state_of(
    crpc::client::call<Resp>& handle) noexcept {
    static_assert(std::is_standard_layout<crpc::client::call<Resp>>::value,
                  "call handle layout changed");
    using state_ptr = std::shared_ptr<crpc::client_detail::call_state<Resp>>;
    return *reinterpret_cast<state_ptr*>(&handle);
}

} // namespace

CRPC_TEST(facade_destroy_client_inside_deadline_callback) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.default_deadline(500ms);
    cb.on_shard(shard);

    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    std::unique_ptr<crpc::client::call<int>> held;
    std::atomic<bool> fired{false};
    std::atomic<bool> destroyed{false};

    // Issuing from a plain thread is allowed; only the deadline timer needs the
    // shard. This keeps the test runnable on nofiber sanitizer builds.
    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (!started.ok()) {
        cli.reset();
        shard.stop();
        shard.join();
        return;
    }
    held = std::make_unique<crpc::client::call<int>>(
        std::move(started).value());
    auto& state = call_state_of(*held);
    CRPC_CHECK(state != nullptr);
    CRPC_CHECK(state != nullptr && state->inner != nullptr);
    if (!state || !state->inner) {
        held.reset();
        cli.reset();
        shard.stop();
        shard.join();
        return;
    }
    // Replace the facade's wake callback with one that destroys the client from
    // inside the deadline poll.
    struct destroy_on_deadline {
        std::unique_ptr<crpc::client>* owner;
        std::atomic<bool>* fired;
        std::atomic<bool>* destroyed;
        void operator()(crpc::rpc::call<int>&) const noexcept {
            fired->store(true, std::memory_order_release);
            owner->reset();
            destroyed->store(true, std::memory_order_release);
        }
    };
    state->inner->then(destroy_on_deadline{&cli, &fired, &destroyed});

    // The deadline timer fires on the shard and destroys the client inside
    // tick_poll(). Before the fix, deadline_thunk() then locked the freed
    // client's timer mutex and tick_poll() kept iterating the freed impl_.
    CRPC_CHECK(crpc_test::wait_for(
        [&destroyed] { return destroyed.load(std::memory_order_acquire); },
        10000));
    CRPC_CHECK(fired.load(std::memory_order_acquire));

    // Let tick_poll() unwind past the callback before stopping the shard.
    std::this_thread::sleep_for(100ms);
    shard.stop();
    shard.join();
    held.reset();
}

CRPC_TEST_MAIN()
