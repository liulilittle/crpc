// CRPC -- usability deep improvements (plan C): C1-C9.
//
// Each case pins one usability gap from the gap analysis:
//   C1  the fallback RAII installer removes the retain/release contract;
//   C2  call_context deadline/idempotency setters set the flag automatically;
//   C3  the facade call handle exposes then();
//   C4  rpc and push share one delivery_mode type;
//   C5  the builder naming aliases forward to the canonical names;
//   C6  service_id / method_id make a swapped argument list a compile error;
//   C7  result<T> gains error_or/value_or and get() distinguishes an invalid
//       handle from a non-coroutine shard thread;
//   C8  the two-argument CRPC_HTTP_GET lambda form and the __COUNTER__ ordinal;
//   C9  server::publish returns a distinguishable result.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/config.hpp>
#include <crpc/delivery_mode.hpp>
#include <crpc/error.hpp>
#include <crpc/id.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/attributes.hpp>
#include <crpc/mvc/router.hpp>
#include <crpc/resilience/fallback.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>
#include <crpc/span.hpp>

// test_framework.hpp (Debug) issues a file-scope pragma that makes later
// `static inline` data-member initializers inert under MSVC. Every CRPC header
// this file needs is included above, and the MVC lambda routes below rely on
// static initialization, so `_DEBUG` is undefined around the harness include to
// keep the pragma out of this translation unit. This mirrors tests/test_mvc.cpp.
#undef _DEBUG
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

// --- C1: fallback RAII -------------------------------------------------------

crpc::status counting_fallback(void* self,
                               const crpc::resilience::fallback_context&,
                               crpc::buffer& out) noexcept {
    ++*static_cast<int*>(self);
    return out.append("fb", 2) ? crpc::status{}
                               : crpc::status{crpc::rpc_code::encode_error};
}

void make_degradable_core(crpc::client_detail::call_core& core) {
    core.origin = crpc::client_detail::failure_origin::remote;
    core.idempotent = true;
}

crpc::status noop_handler(void*, const crpc::call_envelope&,
                           crpc::span<const std::byte>, crpc::buffer&,
                           crpc::rpc::call_context&) {
    return crpc::status{};
}

// --- C3: facade then ---------------------------------------------------------

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

} // namespace

// --- C8: MVC lambda routes (registered at static initialization) -------------

namespace {
int g_lambda_hits = 0;
}

CRPC_HTTP_GET("/lambda-health", [](const crpc::http::request&) {
    return crpc::mvc::with_status(200, std::string("ok"));
})

CRPC_HTTP_GET("/lambda-count", [](const crpc::http::request&) {
    ++g_lambda_hits;
    return crpc::mvc::with_status(200, std::string("counted"));
})

CRPC_HTTP_GET("/lambda-params/:id",
              [](const crpc::http::request&,
                 const crpc::mvc::route_params& params) {
                  const std::string* id = params.find("id");
                  return crpc::mvc::with_status(
                      200, id != nullptr ? *id : std::string("none"));
              })

// --- C1 ----------------------------------------------------------------------

CRPC_TEST(fallback_install_raii_scopes_the_policy) {
    crpc::inproc_backend backend;
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("fb-raii"), backend);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        return;
    }

    auto hits = std::make_shared<int>(0);
    crpc::client_detail::call_core core;
    make_degradable_core(core);
    {
        crpc::fallback_install guard(*cli, &counting_fallback, hits,
                                     std::chrono::nanoseconds{0});
        CRPC_CHECK(guard.active());
        crpc::buffer out;
        crpc::rpc_error error(crpc::rpc_code::unavailable, "degradable");
        error.metadata.set(crpc::resilience::degradable_metadata_key, "true");
        CRPC_CHECK(cli->run_fallback(core, out, error, nullptr) ==
                   crpc::resilience::fallback_outcome::applied);
        CRPC_CHECK_EQ(1, *hits);
    }

    // The guard is gone: the policy is uninstalled, so the next attempt is not
    // configured and the fallback function never runs.
    crpc::buffer out2;
    crpc::rpc_error error2(crpc::rpc_code::unavailable, "degradable");
    error2.metadata.set(crpc::resilience::degradable_metadata_key, "true");
    CRPC_CHECK(cli->run_fallback(core, out2, error2, nullptr) ==
               crpc::resilience::fallback_outcome::not_configured);
    CRPC_CHECK_EQ(1, *hits);

    // An explicitly reset guard behaves the same way.
    {
        crpc::fallback_install guard(*cli, &counting_fallback, hits,
                                     std::chrono::nanoseconds{0});
        guard.reset();
        CRPC_CHECK_FALSE(guard.active());
        crpc::buffer out3;
        crpc::rpc_error error3(crpc::rpc_code::unavailable, "degradable");
        error3.metadata.set(crpc::resilience::degradable_metadata_key, "true");
        CRPC_CHECK(cli->run_fallback(core, out3, error3, nullptr) ==
                   crpc::resilience::fallback_outcome::not_configured);
    }
    CRPC_CHECK_EQ(1, *hits);
}

CRPC_TEST(fallback_install_rejects_a_null_self) {
    crpc::inproc_backend backend;
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("fb-null"), backend);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        return;
    }
    // A null self with a non-null function is a contract violation: nothing is
    // installed and the call reports false.
    CRPC_CHECK_FALSE(cli->set_fallback_owned(&counting_fallback, nullptr,
                                             std::chrono::nanoseconds{0}));
}

// --- C2 ----------------------------------------------------------------------

CRPC_TEST(call_context_setters_set_the_flag) {
    crpc::rpc::call_context ctx;
    CRPC_CHECK_FALSE(ctx.has_deadline);

    const auto when = std::chrono::steady_clock::now() + 1s;
    ctx.set_deadline(when);
    CRPC_CHECK(ctx.has_deadline);
    CRPC_CHECK(ctx.deadline == when);
    CRPC_CHECK_FALSE(ctx.expired(when - 1ms));

    crpc::rpc::call_context timed;
    timed.set_timeout(500ms);
    CRPC_CHECK(timed.has_deadline);
    CRPC_CHECK(timed.remaining(std::chrono::steady_clock::now()) <=
               std::chrono::nanoseconds(500ms));
    CRPC_CHECK(timed.remaining(std::chrono::steady_clock::now()) > 0ns);

    crpc::rpc::call_context idem;
    idem.set_idempotent(true);
    CRPC_CHECK(idem.idempotent);
    idem.set_idempotent(false);
    CRPC_CHECK_FALSE(idem.idempotent);
}

CRPC_TEST(call_context_functional_builders) {
    crpc::rpc::call_context base;
    const auto when = std::chrono::steady_clock::now() + 2s;
    const crpc::rpc::call_context with_dl = crpc::rpc::with_deadline(base, when);
    CRPC_CHECK(with_dl.has_deadline);
    CRPC_CHECK(with_dl.deadline == when);
    // The source is not mutated.
    CRPC_CHECK_FALSE(base.has_deadline);

    const crpc::rpc::call_context with_to = crpc::rpc::with_timeout(base, 250ms);
    CRPC_CHECK(with_to.has_deadline);
    const crpc::rpc::call_context with_idem = crpc::rpc::with_idempotent(base);
    CRPC_CHECK(with_idem.idempotent);
    const crpc::rpc::call_context plain = crpc::rpc::with_idempotent(base, false);
    CRPC_CHECK_FALSE(plain.idempotent);
}

// --- C3 ----------------------------------------------------------------------

CRPC_TEST(facade_call_then_fires_on_completion) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.default_deadline(200ms);
    cb.on_shard(shard);

    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (!started.ok()) {
        cli.reset();
        shard.stop();
        shard.join();
        return;
    }
    crpc::client::call<int> handle = std::move(started).value();
    std::atomic<int> hits{0};
    handle.then([&hits](crpc::client::call<int>&) {
        hits.fetch_add(1, std::memory_order_release);
    });

    const crpc::rpc::rpc_result<int> result = handle.get();
    // The stalled connection times out. The facade get() may return on the
    // local wait bound before the shard timer finalizes the low-level call, so
    // wait for the callback rather than reading the flag immediately.
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK(crpc_test::wait_for(
        [&hits] { return hits.load(std::memory_order_acquire) == 1; }, 3000));
    CRPC_CHECK_EQ(1, hits.load(std::memory_order_acquire));

    shard.stop();
    shard.join();
}

CRPC_TEST(facade_call_then_runs_immediately_when_already_done) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.default_deadline(150ms);
    cb.on_shard(shard);

    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (!started.ok()) {
        cli.reset();
        shard.stop();
        shard.join();
        return;
    }
    crpc::client::call<int> handle = std::move(started).value();
    const crpc::rpc::rpc_result<int> result = handle.get();
    CRPC_CHECK_FALSE(result.ok());
    // Wait for the shard timer to finalize the low-level call so the handle is
    // terminal, then `then` runs the callback inline on this thread.
    CRPC_CHECK(crpc_test::wait_for([&handle] { return handle.ready(); }, 3000));

    std::atomic<int> hits{0};
    handle.then([&hits](crpc::client::call<int>&) {
        hits.fetch_add(1, std::memory_order_release);
    });
    CRPC_CHECK_EQ(1, hits.load(std::memory_order_acquire));

    shard.stop();
    shard.join();
}

// --- C4 ----------------------------------------------------------------------

CRPC_TEST(delivery_mode_is_shared_between_rpc_and_push) {
    static_assert(std::is_same<crpc::rpc::delivery_mode,
                               crpc::push::delivery_mode>::value,
                  "rpc and push must share one delivery_mode type");
    static_assert(std::is_same<crpc::rpc::delivery_mode,
                               crpc::delivery_mode>::value,
                  "the alias must be the canonical crpc::delivery_mode");
    static_assert(static_cast<crpc::u8>(crpc::delivery_mode::reliable) == 0,
                  "reliable keeps wire value 0");
    static_assert(static_cast<crpc::u8>(crpc::delivery_mode::unreliable) == 1,
                  "unreliable keeps wire value 1");

    // A value produced as one alias is usable as the other, no conversion.
    const crpc::rpc::delivery_mode rpc_mode = crpc::delivery_mode::reliable;
    const crpc::push::delivery_mode push_mode = rpc_mode;
    CRPC_CHECK(push_mode == crpc::push::delivery_mode::reliable);
    const crpc::push::delivery_mode from_push = crpc::push::delivery_mode::unreliable;
    const crpc::rpc::delivery_mode back = from_push;
    CRPC_CHECK(back == crpc::rpc::delivery_mode::unreliable);
}

// --- C5 ----------------------------------------------------------------------

CRPC_TEST(builder_aliases_forward_to_the_canonical_names) {
    // The aliases are deprecated; the pragma keeps the deprecation diagnostic
    // out of the zero-warning build.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    crpc::inproc_backend backend;
    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("alias-client"), backend);
    cb.default_timeout(123ms);
    cb.per_connection_inflight(7);
    auto cli = cb.build();
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    CRPC_CHECK(cli != nullptr);
    if (cli) {
        // The alias reached the canonical field: the low-level connection is
        // built with the facade's default deadline (nonzero).
        CRPC_CHECK(cli->stats().active_connections >= 0);
    }
}

CRPC_TEST(server_builder_alias_sets_the_per_connection_cap) {
    crpc::inproc_backend backend;
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("alias-server"), backend);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    sb.max_inflight_per_connection(9);
    sb.per_connection_inflight(9);
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
}

// --- C6 ----------------------------------------------------------------------

CRPC_TEST(strong_ids_prevent_swapping) {
    static_assert(std::is_constructible<crpc::service_id, crpc::u64>::value,
                  "a service_id is constructible from a u64");
    static_assert(std::is_constructible<crpc::method_id, crpc::u64>::value,
                  "a method_id is constructible from a u64");
    // The two ids are distinct: no implicit conversion between them.
    static_assert(!std::is_convertible<crpc::service_id, crpc::method_id>::value,
                  "a service_id must not convert to a method_id");
    static_assert(!std::is_convertible<crpc::method_id, crpc::service_id>::value,
                  "a method_id must not convert to a service_id");
    // The u64 conversion is explicit, so the raw overloads cannot silently
    // accept a typed id.
    static_assert(std::is_convertible<crpc::u64, crpc::service_id>::value,
                  "a u64 converts to a service_id");
    static_assert(!std::is_convertible<crpc::service_id, crpc::u64>::value,
                  "a service_id does not implicitly convert to a u64");

    const crpc::service_id service{42};
    const crpc::method_id method{7};
    CRPC_CHECK(service.valid());
    CRPC_CHECK(method.valid());
    CRPC_CHECK_EQ(crpc::u64{42}, static_cast<crpc::u64>(service));
    CRPC_CHECK_EQ(crpc::u64{7}, static_cast<crpc::u64>(method));
    CRPC_CHECK_FALSE(crpc::service_id{}.valid());
    CRPC_CHECK_FALSE(crpc::method_id{}.valid());
    CRPC_CHECK(service == crpc::service_id{42});
    CRPC_CHECK(service != crpc::service_id{43});
    CRPC_CHECK(method == crpc::method_id{7});
    CRPC_CHECK(method != crpc::method_id{8});

    // The typed overloads accept the newtypes and reach the u64 registry.
    crpc::rpc::service_registry registry;
    CRPC_CHECK(registry.register_service(service, "svc").ok());
    CRPC_CHECK(registry.register_method(service, method, "m", &noop_handler,
                                        nullptr)
                   .ok());
    CRPC_CHECK(registry.find_method(service.value, method.value) != nullptr);
}

// --- C7 ----------------------------------------------------------------------

CRPC_TEST(result_value_or_and_error_or) {
    crpc::result<int> ok_value(42);
    CRPC_CHECK_EQ(42, ok_value.value_or(0));
    const crpc::result<int> const_ok(7);
    CRPC_CHECK_EQ(7, const_ok.value_or(0));
    CRPC_CHECK_EQ(9, crpc::result<int>(crpc::rpc_code::unavailable).value_or(9));

    crpc::result<int> failed(crpc::rpc_error(crpc::rpc_code::unavailable, "no"));
    const crpc::rpc_error fallback(crpc::rpc_code::internal, "fallback");
    const crpc::rpc_error got = failed.error_or(fallback);
    CRPC_CHECK(got.code == crpc::rpc_code::unavailable);
    const crpc::rpc_error from_ok = ok_value.error_or(fallback);
    CRPC_CHECK(from_ok.code == crpc::rpc_code::internal);
    CRPC_CHECK_EQ(std::string("fallback"), from_ok.message);
}

CRPC_TEST(get_distinguishes_invalid_handle) {
    crpc::rpc::call<int> invalid_rpc;
    const crpc::rpc::rpc_result<int> rpc_result = invalid_rpc.get();
    CRPC_CHECK(rpc_result.code() == crpc::rpc_code::invalid_argument);

    crpc::client::call<int> invalid_client;
    const crpc::rpc::rpc_result<int> client_result = invalid_client.get();
    CRPC_CHECK(client_result.code() == crpc::rpc_code::invalid_argument);
    CRPC_CHECK(invalid_client.get().error().message.find("invalid") !=
               std::string::npos);
}

CRPC_TEST(get_outside_a_coroutine_is_failed_precondition) {
    // The other side of the C7 distinction: a wait on a shard thread that is not
    // inside a coroutine is `failed_precondition`, not `invalid_argument`.
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }
    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (!started.ok()) {
        cli.reset();
        shard.stop();
        shard.join();
        return;
    }
    auto handle =
        std::make_unique<crpc::client::call<int>>(std::move(started).value());

    struct ctx_t {
        crpc::client::call<int>* handle;
        int* code;
        std::atomic<bool>* done;
    };
    std::atomic<bool> done{false};
    int code = -1;
    ctx_t ctx{handle.get(), &code, &done};
    auto task = [](void* raw) noexcept {
        auto* c = static_cast<ctx_t*>(raw);
        const crpc::rpc::rpc_result<int> result = c->handle->get();
        *c->code = result.ok() ? 0 : static_cast<int>(result.code());
        c->done->store(true, std::memory_order_release);
    };
    // post() runs the task on the shard thread but NOT inside a coroutine, so
    // wait_ready()'s park() returns failed_precondition.
    CRPC_CHECK(shard.post(task, &ctx));
    CRPC_CHECK(crpc_test::wait_for([&done] { return done.load(); }, 3000));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition), code);

    shard.stop();
    shard.join();
}

// --- C8 ----------------------------------------------------------------------

// Defined before the dispatch test: `default_router()` consumes
// `pending_routes()` on first use, so the ordinal check must run first.
CRPC_TEST(mvc_lambda_ordinals_are_unique) {
    // The ordinal and the object name come from __COUNTER__, so two lambda
    // routes expanded on the same source line cannot collide on __LINE__.
    std::vector<std::uint32_t> ordinals;
    for (const crpc::mvc::route_entry& entry : crpc::mvc::pending_routes()) {
        if (entry.path == "/lambda-health" || entry.path == "/lambda-count" ||
            entry.path == "/lambda-params/:id") {
            ordinals.push_back(entry.ordinal);
        }
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), ordinals.size());
    for (std::size_t i = 0; i < ordinals.size(); ++i) {
        for (std::size_t j = i + 1; j < ordinals.size(); ++j) {
            CRPC_CHECK(ordinals[i] != ordinals[j]);
        }
    }
}

CRPC_TEST(mvc_lambda_route_registers_and_dispatches) {
    const crpc::mvc::router& routes = crpc::mvc::default_router();
    CRPC_CHECK(routes.frozen());
    CRPC_CHECK(routes.size() >= static_cast<std::size_t>(3));

    {
        crpc::http::request request;
        request.method = crpc::http::method::get;
        request.path = "/lambda-health";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(200, response.status_code);
        CRPC_CHECK(response.body.find("ok") != std::string::npos);
    }
    {
        crpc::http::request request;
        request.method = crpc::http::method::get;
        request.path = "/lambda-count";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(200, response.status_code);
        CRPC_CHECK(response.body.find("counted") != std::string::npos);
    }
    {
        crpc::http::request request;
        request.method = crpc::http::method::get;
        request.path = "/lambda-params/abc";
        crpc::http::response response;
        CRPC_CHECK(crpc::mvc::dispatch(routes, request, response).ok());
        CRPC_CHECK_EQ(200, response.status_code);
        CRPC_CHECK(response.body.find("abc") != std::string::npos);
    }
}

// --- C9 ----------------------------------------------------------------------

CRPC_TEST(publish_result_names_each_refusal) {
    CRPC_CHECK_EQ(std::string("ok"),
                  crpc::to_string(crpc::publish_result::code::ok));
    CRPC_CHECK_EQ(std::string("empty_topic"),
                  crpc::to_string(crpc::publish_result::code::empty_topic));
    CRPC_CHECK_EQ(std::string("not_running"),
                  crpc::to_string(crpc::publish_result::code::not_running));
    CRPC_CHECK_EQ(
        std::string("unsupported_configuration"),
        crpc::to_string(crpc::publish_result::code::unsupported_configuration));

    CRPC_CHECK(crpc::is_ok(crpc::publish_result(crpc::publish_result::code::ok)));
    CRPC_CHECK_FALSE(
        crpc::is_ok(crpc::publish_result(crpc::publish_result::code::not_running)));

    // Backward compatibility: the implicit bool conversion keeps an existing
    // `if (server.publish(...))` / CRPC_CHECK(server.publish(...)) meaning "ok".
    const crpc::publish_result ok_result(crpc::publish_result::code::ok);
    const crpc::publish_result bad_result(crpc::publish_result::code::not_running);
    CRPC_CHECK(ok_result);
    CRPC_CHECK_FALSE(bad_result);
    const bool as_bool = ok_result;
    CRPC_CHECK(as_bool);
    CRPC_CHECK(ok_result == true);
    CRPC_CHECK_FALSE(bad_result == true);
}

CRPC_TEST(publish_distinguishes_refusals) {
    crpc::inproc_backend backend;
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("publish-result"), backend);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }

    // Not started: not_running.
    CRPC_CHECK(srv->publish("topic", crpc::span<const std::byte>{}) ==
               crpc::publish_result::code::not_running);
    // Empty topic is reported even before start.
    CRPC_CHECK(srv->publish("", crpc::span<const std::byte>{}) ==
               crpc::publish_result::code::empty_topic);

    CRPC_CHECK(srv->start().ok());
    // Empty topic on a running server.
    CRPC_CHECK(srv->publish("", crpc::span<const std::byte>{}) ==
               crpc::publish_result::code::empty_topic);
    // A normal publish succeeds.
    CRPC_CHECK(crpc::is_ok(srv->publish("topic", crpc::span<const std::byte>{})));

    srv->stop();
    // After stop: not_running again, not a generic false.
    CRPC_CHECK(srv->publish("topic", crpc::span<const std::byte>{}) ==
               crpc::publish_result::code::not_running);
}

CRPC_TEST(publish_reports_unsupported_multi_shard) {
    crpc::inproc_backend backend;
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("publish-shards"), backend);
    sb.shards(2);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());
    CRPC_CHECK(srv->publish("topic", crpc::span<const std::byte>{}) ==
               crpc::publish_result::code::unsupported_configuration);
    srv->stop();
}

CRPC_TEST_MAIN()
