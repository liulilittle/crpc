// CRPC -- end-to-end tests for the server, client facade and shard runtime.
//
// The in-process backend is used for every behavioural test; the IOCP backend
// is exercised on Windows when it can bind a local port.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

constexpr crpc::u64 kService = 100;
constexpr crpc::u64 kEcho = 1;
constexpr crpc::u64 kSlow = 2;
constexpr crpc::u64 kFlaky = 3;
constexpr crpc::u64 kFail = 4;

struct service_state {
    std::atomic<int> echo_calls{0};
    std::atomic<int> slow_calls{0};
    std::atomic<int> flaky_calls{0};
    std::atomic<int> fail_calls{0};
    std::atomic<int> canceled_observed{0};
    std::atomic<int> flaky_failures_remaining{1};

    int echo(const int& value) {
        echo_calls.fetch_add(1, std::memory_order_relaxed);
        return value + 1;
    }

    int slow(const int& value, crpc::rpc::call_context& ctx) {
        slow_calls.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(5ms);
            if (ctx.has_deadline && ctx.expired(std::chrono::steady_clock::now())) {
                canceled_observed.store(1, std::memory_order_relaxed);
            }
        }
        return value + 100;
    }

    static crpc::status raw_flaky(void* self, const crpc::call_envelope&,
                                  crpc::span<const std::byte>, crpc::buffer& out,
                                  crpc::rpc::call_context&) {
        auto* state = static_cast<service_state*>(self);
        state->flaky_calls.fetch_add(1, std::memory_order_relaxed);
        if (state->flaky_failures_remaining.fetch_sub(1, std::memory_order_acq_rel) > 0) {
            return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "flaky")};
        }
        return crpc::rpc::call_codec<int>::encode(42, out);
    }

    static crpc::status raw_fail(void* self, const crpc::call_envelope&,
                                 crpc::span<const std::byte>, crpc::buffer&,
                                 crpc::rpc::call_context&) {
        auto* state = static_cast<service_state*>(self);
        state->fail_calls.fetch_add(1, std::memory_order_relaxed);
        return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "always")};
    }
};

using clock_type = std::chrono::steady_clock;

void register_services(crpc::server::builder& builder, service_state& service) {
    builder.add_service(kService, "svc");
    builder.template add_method<service_state, &service_state::echo>(
        kService, kEcho, "echo", service);
    builder.template add_method<service_state, &service_state::slow>(
        kService, kSlow, "slow", service);
    builder.add_raw_method(kService, kFlaky, "flaky", &service_state::raw_flaky,
                           &service);
    builder.add_raw_method(kService, kFail, "fail", &service_state::raw_fail,
                           &service);
}

struct fixture_config {
    std::size_t server_workers = 8;
    std::size_t per_connection = 1024;
    std::size_t per_service = 65536;
    std::chrono::milliseconds drain{2000};
    std::chrono::milliseconds overload_retry{10};
    std::chrono::nanoseconds default_deadline{1000ms};
    bool default_idempotent = false;
    std::size_t pool_size = 1;
    std::size_t max_inflight = 1024;
    std::chrono::milliseconds idle_eviction{0};
    bool configure_retry = false;
    crpc::resilience::retry_policy retry{};
    bool configure_breaker = false;
    crpc::resilience::breaker_config breaker{};
};

struct fixture {
    crpc::inproc_backend backend;
    service_state service;
    std::unique_ptr<crpc::runtime> shard;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    std::string name = "crpc-test";

    ~fixture() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
        if (shard) {
            shard->stop();
            shard->join();
        }
    }
};

bool start_inproc(fixture& f, const fixture_config& cfg) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(f.name), f.backend);
    register_services(sb, f.service);
    sb.handler_workers(cfg.server_workers);
    sb.per_connection_inflight(cfg.per_connection);
    sb.per_service_inflight(cfg.per_service);
    sb.drain_timeout(cfg.drain);
    sb.overload_retry_after(cfg.overload_retry);
    f.srv = sb.build();
    if (!f.srv) {
        return false;
    }
    if (!f.srv->start().ok()) {
        return false;
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    f.shard = std::make_unique<crpc::runtime>(ro);
    f.shard->start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(f.name), f.backend);
    cb.pool_size(cfg.pool_size);
    cb.max_inflight_per_connection(cfg.max_inflight);
    cb.default_deadline(cfg.default_deadline);
    cb.default_idempotent(cfg.default_idempotent);
    cb.on_shard(*f.shard);
    cb.idle_eviction(cfg.idle_eviction);
    if (cfg.configure_retry) {
        cb.retry(cfg.retry);
    }
    if (cfg.configure_breaker) {
        cb.breaker(cfg.breaker);
    }
    f.cli = cb.build();
    return f.cli != nullptr;
}

// A coroutine body plus a completion flag. The owning test waits for `done`
// before reading values written by the coroutine.
struct coroutine_job {
    std::function<void()> body;
    std::atomic<bool> done{false};
};

void coroutine_entry(void* ctx) {
    auto* job = static_cast<coroutine_job*>(ctx);
    job->body();
    job->done.store(true, std::memory_order_release);
}

bool run_on_shard(crpc::runtime& shard, std::function<void()> body,
                  int timeout_ms = 5000) {
    auto* job = new coroutine_job{std::move(body), {}};
    if (!shard.spawn(&coroutine_entry, job)) {
        delete job;
        return false;
    }
    const bool finished = crpc_test::wait_for(
        [job] { return job->done.load(std::memory_order_acquire); }, timeout_ms);
    delete job;
    return finished;
}

// --- runtime primitives ------------------------------------------------------

void increment_task(void* ctx) noexcept {
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

void set_timer_flag(void* ctx) noexcept {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

CRPC_TEST(runtime_post_submit_timer_and_stop) {
    crpc::runtime_options options;
    options.start_handler_pool = true;
    options.handler_workers = 2;
    options.handler_stack_bytes = 64 * 1024;
    crpc::runtime shard(options);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    std::atomic<int> inbox{0};
    std::atomic<int> handler{0};
    std::atomic<bool> fired{false};
    CRPC_CHECK(shard.post(&increment_task, &inbox));
    CRPC_CHECK(shard.submit(&increment_task, &handler));
    CRPC_CHECK(shard.arm_timer(&fired, shard.now_ns() + 10'000'000ull,
                               &set_timer_flag, &fired));

    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            return inbox.load() == 1 && handler.load() == 1 && fired.load();
        },
        3000));
    CRPC_CHECK_EQ(1, inbox.load());
    CRPC_CHECK_EQ(1, handler.load());
    CRPC_CHECK(fired.load());

    shard.stop();
    shard.stop();
    shard.join();
    CRPC_CHECK(!shard.running());
    CRPC_CHECK(!shard.cancel_timer(&fired));
}

// --- tests -------------------------------------------------------------------

CRPC_TEST(inproc_echo) {
    fixture f;
    fixture_config cfg;
    CRPC_CHECK(start_inproc(f, cfg));
    CRPC_CHECK(f.shard->loop_mode());

    int value = -1;
    int code = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(41);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
            code = 0;
        } else {
            code = static_cast<int>(result.code());
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(0, code);
    CRPC_CHECK_EQ(42, value);
}

CRPC_TEST(sequential_1000_calls) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    int completed = 0;
    int failed = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kEcho);
            for (int i = 0; i < 1000; ++i) {
                auto started = stub.call(i);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                auto handle = std::move(started).value();
                auto result = handle.get();
                if (result.ok() && result.value() == i + 1) {
                    ++completed;
                } else {
                    ++failed;
                }
            }
        },
        20000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(1000, completed);
    CRPC_CHECK_EQ(0, failed);
    CRPC_CHECK_EQ(1000, f.service.echo_calls.load());
}

CRPC_TEST(concurrent_100_calls) {
    fixture f;
    fixture_config cfg;
    cfg.server_workers = 64;
    cfg.default_deadline = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    int started_count = 0;
    int completed = 0;
    int failed = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kEcho);
            std::vector<crpc::client::call<int>> calls;
            calls.reserve(100);
            for (int i = 0; i < 100; ++i) {
                auto started = stub.call(i);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                calls.push_back(std::move(started).value());
                ++started_count;
            }
            int index = 0;
            for (auto& handle : calls) {
                auto result = handle.get();
                if (result.ok() && result.value() == index + 1) {
                    ++completed;
                } else {
                    ++failed;
                }
                ++index;
            }
        },
        20000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(100, started_count);
    CRPC_CHECK_EQ(100, completed);
    CRPC_CHECK_EQ(0, failed);
}

CRPC_TEST(deadline_exceeded_and_handler_observes) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 1s;
    CRPC_CHECK(start_inproc(f, cfg));

    int code = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kSlow);
        crpc::rpc::call_context ctx;
        ctx.has_deadline = true;
        ctx.deadline = clock_type::now() + 40ms;
        auto started = stub.call(1, ctx);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        code = result.ok() ? 0 : static_cast<int>(result.code());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded), code);
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.canceled_observed.load() > 0; }, 2000));
}

CRPC_TEST(per_connection_overload) {
    fixture f;
    fixture_config cfg;
    cfg.per_connection = 1;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    int first_code = -1;
    int second_code = -1;
    int retry_after = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kSlow);
            auto first = stub.call(7);
            auto second = stub.call(8);
            if (!second.ok()) {
                second_code = static_cast<int>(second.code());
            } else {
                auto handle = std::move(second).value();
                auto result = handle.get();
                second_code = result.ok() ? 0 : static_cast<int>(result.code());
                if (!result.ok()) {
                    retry_after = static_cast<int>(result.error().retry_after.count());
                }
            }
            if (!first.ok()) {
                first_code = static_cast<int>(first.code());
            } else {
                auto handle = std::move(first).value();
                auto result = handle.get();
                first_code = result.ok() ? 0 : static_cast<int>(result.code());
            }
        },
        10000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload), second_code);
    CRPC_CHECK(retry_after > 0);
    CRPC_CHECK_EQ(0, first_code);
    CRPC_CHECK(f.srv->stats().rejected_overload >= 1);
}

CRPC_TEST(graceful_shutdown_drains_and_rejects) {
    fixture f;
    fixture_config cfg;
    cfg.drain = 2s;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    std::atomic<bool> started{false};
    int in_flight_code = -1;
    auto* thread = new std::thread([&] {
        const bool ran = run_on_shard(
            *f.shard,
            [&] {
                auto stub = f.cli->service<int, int>(kService, kSlow);
                auto result_handle = stub.call(5);
                started.store(true, std::memory_order_release);
                if (!result_handle.ok()) {
                    in_flight_code = static_cast<int>(result_handle.code());
                    return;
                }
                auto handle = std::move(result_handle).value();
                auto result = handle.get();
                in_flight_code = result.ok() ? 0 : static_cast<int>(result.code());
            },
            10000);
        (void)ran;
    });

    CRPC_CHECK(crpc_test::wait_for(
        [&started] { return started.load(std::memory_order_acquire); }, 2000));
    // Let the shard hand the request to a handler before shutdown starts.
    std::this_thread::sleep_for(50ms);
    const auto begin = clock_type::now();
    f.srv->stop();
    const auto elapsed = clock_type::now() - begin;
    thread->join();
    delete thread;

    CRPC_CHECK(in_flight_code == 0);
    CRPC_CHECK(elapsed < 2s);
    CRPC_CHECK(!f.srv->running());

    int after_code = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started_call = stub.call(1);
        after_code = started_call.ok()
                         ? 0
                         : static_cast<int>(started_call.code());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down), after_code);
}

CRPC_TEST(retry_idempotent_only) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_retry = true;
    cfg.retry.max_attempts = 2;
    cfg.retry.initial_backoff = 1ms;
    cfg.retry.max_backoff = 4ms;
    cfg.retry.retry_budget = 10.0;
    CRPC_CHECK(start_inproc(f, cfg));

    int idempotent_value = -1;
    int idempotent_attempts = 0;
    f.service.flaky_failures_remaining.store(1);
    const bool first_ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kFlaky);
        crpc::rpc::call_context ctx;
        ctx.idempotent = true;
        auto started = stub.call(0, ctx);
        if (!started.ok()) {
            return;
        }
        auto handle = std::move(started).value();
        idempotent_attempts = static_cast<int>(handle.attempts());
        auto result = handle.get();
        if (result.ok()) {
            idempotent_value = result.value();
        }
        idempotent_attempts = static_cast<int>(handle.attempts());
    });
    CRPC_CHECK(first_ran);
    CRPC_CHECK_EQ(42, idempotent_value);
    CRPC_CHECK_EQ(2, idempotent_attempts);
    CRPC_CHECK_EQ(2, f.service.flaky_calls.load());

    f.service.flaky_calls.store(0);
    f.service.flaky_failures_remaining.store(1);
    int non_idempotent_code = -1;
    int non_idempotent_attempts = 0;
    const bool second_ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kFlaky);
        crpc::rpc::call_context ctx;
        ctx.idempotent = false;
        auto started = stub.call(0, ctx);
        if (!started.ok()) {
            non_idempotent_code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        non_idempotent_code =
            result.ok() ? 0 : static_cast<int>(result.code());
        non_idempotent_attempts = static_cast<int>(handle.attempts());
    });
    CRPC_CHECK(second_ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable), non_idempotent_code);
    CRPC_CHECK_EQ(1, non_idempotent_attempts);
    CRPC_CHECK_EQ(1, f.service.flaky_calls.load());
}

CRPC_TEST(breaker_opens_after_five_failures) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_breaker = true;
    cfg.breaker.min_requests = 5;
    cfg.breaker.failure_ratio_threshold = 0.5;
    cfg.breaker.open_duration = 5s;
    cfg.retry.max_attempts = 1;
    CRPC_CHECK(start_inproc(f, cfg));

    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kFail);
        for (int i = 0; i < 5; ++i) {
            auto started = stub.call(i);
            if (!started.ok()) {
                continue;
            }
            auto handle = std::move(started).value();
            (void)handle.get();
        }
        auto started = stub.call(99);
        if (started.ok()) {
            auto handle = std::move(started).value();
            (void)handle.get();
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(5, f.service.fail_calls.load());
    CRPC_CHECK(f.cli->stats().breaker_rejections >= 1);
}

CRPC_TEST(pool_exhaustion_does_not_stall) {
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 1;
    cfg.max_inflight = 1;
    cfg.default_deadline = 2s;
    cfg.retry.max_attempts = 1;
    CRPC_CHECK(start_inproc(f, cfg));

    int first_code = -1;
    int second_code = -1;
    const auto begin = clock_type::now();
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kSlow);
            auto first = stub.call(1);
            auto second = stub.call(2);
            second_code = second.ok() ? 0 : static_cast<int>(second.code());
            if (first.ok()) {
                auto handle = std::move(first).value();
                auto result = handle.get();
                first_code = result.ok() ? 0 : static_cast<int>(result.code());
            } else {
                first_code = static_cast<int>(first.code());
            }
        },
        10000);
    const auto elapsed = clock_type::now() - begin;
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted), second_code);
    CRPC_CHECK_EQ(0, first_code);
    CRPC_CHECK(elapsed < 2s);
}

CRPC_TEST(no_leak_after_close) {
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 2;
    CRPC_CHECK(start_inproc(f, cfg));

    int value = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(5);
        if (!started.ok()) {
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(6, value);

    f.cli->close();
    f.srv->stop();
    const crpc::client::statistics stats = f.cli->stats();
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), stats.active_connections);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), stats.in_flight_calls);
    CRPC_CHECK(f.srv->stats().active_connections == 0);
    CRPC_CHECK(f.srv->stats().in_flight_calls == 0);
}

#if defined(_WIN32)
CRPC_TEST(iocp_echo_when_available) {
    crpc::iocp_backend backend;
    service_state service;

    std::unique_ptr<crpc::server> srv;
    crpc::endpoint bound;
    for (crpc::u16 port = 43210; port < 43220; ++port) {
        crpc::server::builder sb;
        sb.add_listener(crpc::endpoint::tcp("127.0.0.1", port), backend);
        register_services(sb, service);
        sb.handler_workers(8);
        sb.drain_timeout(1000ms);
        auto candidate = sb.build();
        if (!candidate) {
            continue;
        }
        if (!candidate->start().ok()) {
            continue;
        }
        srv = std::move(candidate);
        bound = crpc::endpoint::tcp("127.0.0.1", port);
        break;
    }
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(bound, backend);
    cb.default_deadline(3s);
    cb.plain_thread_timeout(5s);
    cb.on_shard(shard);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        srv->stop();
        return;
    }

    int value = -1;
    int code = -1;
    const bool ran = run_on_shard(shard, [&] {
        auto stub = cli->service<int, int>(kService, kEcho);
        auto started = stub.call(9);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
            code = 0;
        } else {
            code = static_cast<int>(result.code());
        }
    }, 10000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(0, code);
    CRPC_CHECK_EQ(10, value);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}
#endif

} // namespace

CRPC_TEST_MAIN()
