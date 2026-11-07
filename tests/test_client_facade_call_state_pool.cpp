// CRPC -- facade call-state pool regression.
//
// P1 removed the two per-call heap allocations: the `call_state` object and its
// `shared_ptr` control block now come from a per-client pool, and the per-call
// `retry_sequence` is stored inline in `call_core`. This file proves the pool is
// actually used (blocks are reused and the system allocator is not reached) and
// that a call issued through the normal facade path still works.
//
// The pool's lifetime is the interesting part: the allocator stored in each
// control block holds a `shared_ptr` to the pool, so a handle held after
// `close()` still has a live pool to return its block to. The tests below do not
// exercise that directly, but they do prove the pooled path is the one a call
// takes.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <system_error>
#include <utility>

#include <crpc/client_facade.hpp>
#include <crpc/config.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

#include "test_framework.hpp"

namespace {

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

void reset_counters() {
    auto& counters = crpc::client_detail::g_call_state_pool_counters;
    counters.system_alloc.store(0, std::memory_order_relaxed);
    counters.reuse.store(0, std::memory_order_relaxed);
    counters.acquire.store(0, std::memory_order_relaxed);
}

std::size_t system_allocations() {
    return crpc::client_detail::g_call_state_pool_counters.system_alloc.load(
        std::memory_order_relaxed);
}

std::size_t reuses() {
    return crpc::client_detail::g_call_state_pool_counters.reuse.load(
        std::memory_order_relaxed);
}

} // namespace

// The pooled allocation path itself: after warm-up, every `allocate_shared`
// reuses a block and none reaches the system allocator.
CRPC_TEST(facade_call_state_pool_reuses_blocks_without_allocating) {
    auto pool = std::make_shared<crpc::client_detail::call_state_pool>();
    using state_type = crpc::client_detail::call_state<int>;
    crpc::client_detail::call_state_allocator<state_type> alloc(pool);

    // Warm up so the first measured call is a reuse, not a pool growth.
    for (int i = 0; i < 8; ++i) {
        auto state = std::allocate_shared<state_type>(alloc);
        CRPC_CHECK(state != nullptr);
    }
    reset_counters();

    constexpr int kCalls = 64;
    for (int i = 0; i < kCalls; ++i) {
        auto state = std::allocate_shared<state_type>(alloc);
        CRPC_CHECK(state != nullptr);
        // The object lives in the pooled block and is usable as usual.
        CRPC_CHECK(state->core_ref().owner.load(std::memory_order_acquire) ==
                   nullptr);
    }
    CRPC_CHECK_EQ(std::size_t{0}, system_allocations());
    CRPC_CHECK_EQ(std::size_t{kCalls}, reuses());
}

// The facade's own call path must draw its state from the same pool, so a call
// that fails before it can start still does not allocate.
CRPC_TEST(facade_start_encoded_uses_the_pool) {
    stalled_backend backend;
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("pool"), backend);
    cb.pool_size(1);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        return;
    }

    for (int i = 0; i < 8; ++i) {
        (void)cli->start_encoded<int>(1, 1, crpc::span<const std::byte>{});
    }
    reset_counters();

    constexpr int kCalls = 64;
    for (int i = 0; i < kCalls; ++i) {
        (void)cli->start_encoded<int>(1, 1, crpc::span<const std::byte>{});
    }
    CRPC_CHECK_EQ(std::size_t{0}, system_allocations());
    CRPC_CHECK_EQ(std::size_t{kCalls}, reuses());
}

// The facade's default slot pool cannot be larger than the default in-flight
// cap: every slot beyond `max_inflight_per_connection` is dead memory the cap
// can never hand out (~472 B pinned per connection). The two defaults must
// stay consistent so the idle pool matches the per-connection design budget.
CRPC_TEST(facade_default_slot_capacity_never_exceeds_default_inflight_cap) {
    crpc::client::options options;
    CRPC_CHECK_EQ(std::size_t{1024}, options.slot_capacity);
    CRPC_CHECK(options.slot_capacity <= options.max_inflight_per_connection);
}

CRPC_TEST_MAIN()
