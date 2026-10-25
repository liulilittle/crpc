// CRPC -- diagnostics tests.
//
// Task 1 of the diagnostics workflow: `rpc_error` carries the origin of a
// terminal failure and the number of attempts the logical call made. The
// `failure_origin` enum lives in `error.hpp` (the core error header) so that
// `rpc_error` can name it without pulling in the client facade.
#include <crpc/diagnostics.hpp>
#include <crpc/error.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <crpc/client_facade.hpp>
#include <crpc/observability/interceptor.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

CRPC_TEST(rpc_error_origin_and_attempts_default) {
    const crpc::rpc_error error;
    CRPC_CHECK(error.origin == crpc::failure_origin::local_lifecycle);
    CRPC_CHECK(error.attempts == 0);
}

CRPC_TEST(rpc_error_origin_and_attempts_round_trip) {
    crpc::rpc_error error;
    error.origin = crpc::failure_origin::remote;
    error.attempts = 3;
    CRPC_CHECK(error.origin == crpc::failure_origin::remote);
    CRPC_CHECK(error.attempts == 3);
}

CRPC_TEST(rpc_error_origin_and_attempts_survive_copy) {
    crpc::rpc_error error;
    error.origin = crpc::failure_origin::timeout;
    error.attempts = 7;
    const crpc::rpc_error copy = error;
    CRPC_CHECK(copy.origin == crpc::failure_origin::timeout);
    CRPC_CHECK(copy.attempts == 7);
}

CRPC_TEST(rpc_error_failure_origin_is_core_enum) {
    // `failure_origin` must be reachable from `error.hpp` alone: the enum is a
    // core error concept, not a client-facade detail.
    crpc::failure_origin origin = crpc::failure_origin::endpoint_sent;
    CRPC_CHECK(origin == crpc::failure_origin::endpoint_sent);
}

// --- Task 2: to_string and classification predicates -------------------------

CRPC_TEST(diagnostics_to_string_includes_code_message_origin_and_attempts) {
    crpc::rpc_error error(crpc::rpc_code::connection_refused, "no route to host");
    error.origin = crpc::failure_origin::endpoint_open;
    error.attempts = 3;

    const std::string text = crpc::to_string(error);
    CRPC_CHECK(text.find("connection_refused") != std::string::npos);
    CRPC_CHECK(text.find("no route to host") != std::string::npos);
    CRPC_CHECK(text.find("endpoint_open") != std::string::npos);
    CRPC_CHECK(text.find("attempts=3") != std::string::npos);
}

CRPC_TEST(diagnostics_to_string_defaults) {
    const crpc::rpc_error error;
    const std::string text = crpc::to_string(error);
    CRPC_CHECK(text.find("ok") != std::string::npos);
    CRPC_CHECK(text.find("local_lifecycle") != std::string::npos);
    CRPC_CHECK(text.find("attempts=0") != std::string::npos);
}

CRPC_TEST(diagnostics_classification_predicate_matrix) {
    struct expectation {
        crpc::failure_origin origin;
        bool local;
        bool network;
        bool timeout;
        bool circuit_breaker;
        bool rate_limiter;
        bool fallback;
        bool server;
    };
    // Ruling 16 mapping: `local_gate` is rate limiting + bulkhead,
    // `local_capacity` is connection-pool capacity, `endpoint_open` is a refused
    // connect (breaker open), and `endpoint_pre_send`/`endpoint_sent` are
    // network failures after a connection was established.
    const expectation cases[] = {
        {crpc::failure_origin::local_lifecycle, true,  false, false, false, false, true,  false},
        {crpc::failure_origin::local_gate,       false, false, false, false, true,  false, false},
        {crpc::failure_origin::local_capacity,   true,  false, false, false, false, false, false},
        {crpc::failure_origin::endpoint_open,    false, false, false, true,  false, false, false},
        {crpc::failure_origin::endpoint_pre_send,false, true,  false, false, false, false, false},
        {crpc::failure_origin::endpoint_sent,    false, true,  false, false, false, false, false},
        {crpc::failure_origin::remote,           false, false, false, false, false, false, true},
        {crpc::failure_origin::timeout,          false, false, true,  false, false, false, false},
        {crpc::failure_origin::cancel,           false, false, false, false, false, false, false},
    };

    for (const expectation& c : cases) {
        crpc::rpc_error error;
        error.origin = c.origin;
        CRPC_CHECK_EQ(c.local, crpc::is_local(error));
        CRPC_CHECK_EQ(c.network, crpc::is_network(error));
        CRPC_CHECK_EQ(c.timeout, crpc::is_timeout(error));
        CRPC_CHECK_EQ(c.circuit_breaker, crpc::is_circuit_breaker(error));
        CRPC_CHECK_EQ(c.rate_limiter, crpc::is_rate_limiter(error));
        CRPC_CHECK_EQ(c.fallback, crpc::is_fallback(error));
        CRPC_CHECK_EQ(c.server, crpc::is_server(error));
    }
}

CRPC_TEST(diagnostics_is_fallback_matches_lifecycle_only) {
    // A successful fallback produces no error, and there is no dedicated
    // fallback origin, so `is_fallback` can only key on `local_lifecycle`. It is
    // a lifecycle marker, not a true fallback detector.
    crpc::rpc_error error;
    error.origin = crpc::failure_origin::local_lifecycle;
    CRPC_CHECK(crpc::is_fallback(error));

    error.origin = crpc::failure_origin::local_gate;
    CRPC_CHECK_FALSE(crpc::is_fallback(error));
    error.origin = crpc::failure_origin::local_capacity;
    CRPC_CHECK_FALSE(crpc::is_fallback(error));
}

CRPC_TEST(diagnostics_predicates_are_origin_based_not_code_based) {
    // A remote origin carrying a local-looking code is still a server failure:
    // the predicates read `origin`, not `code`.
    crpc::rpc_error error(crpc::rpc_code::unavailable, "server rejected");
    error.origin = crpc::failure_origin::remote;
    CRPC_CHECK(crpc::is_server(error));
    CRPC_CHECK_FALSE(crpc::is_local(error));
}

// --- Task 3: completion hook -------------------------------------------------
//
// `diagnostics::set_completion_hook` installs a process-wide notification and
// `diagnostics::on_completion` invokes it exactly once per call, outside any
// library lock. `response<Resp>::get()` notifies the hook the first time it
// observes a terminal outcome (success, failure or cancel); repeat reads of the
// same response do not notify again.

namespace {

struct completion_probe {
    int calls = 0;
    crpc::rpc_code last_code = crpc::rpc_code::internal;
    crpc::failure_origin last_origin = crpc::failure_origin::remote;
    std::uint32_t last_attempts = 0;
};

void record_completion(const crpc::rpc_error& err, void* self) noexcept {
    auto* probe = static_cast<completion_probe*>(self);
    ++probe->calls;
    probe->last_code = err.code;
    probe->last_origin = err.origin;
    probe->last_attempts = err.attempts;
}

// Installs no hook at construction and clears the process-wide hook at
// destruction, so a failing test cannot leak a dangling probe into the next
// case.
struct completion_hook_scope {
    completion_hook_scope() noexcept {
        crpc::diagnostics::set_completion_hook(nullptr, nullptr);
    }
    ~completion_hook_scope() {
        crpc::diagnostics::set_completion_hook(nullptr, nullptr);
    }
    completion_hook_scope(const completion_hook_scope&) = delete;
    completion_hook_scope& operator=(const completion_hook_scope&) = delete;
};

// A hook that re-enters `set_completion_hook`. It deadlocks if `on_completion`
// calls the hook while holding the hook lock.
void clear_hook_on_completion(const crpc::rpc_error&, void* self) noexcept {
    auto* probe = static_cast<completion_probe*>(self);
    ++probe->calls;
    crpc::diagnostics::set_completion_hook(nullptr, nullptr);
}

// Clears the process-wide default interceptor chain for the duration of a test,
// so an installed chain cannot leak into the next case.
struct default_chain_scope {
    default_chain_scope() noexcept {
        crpc::obs::set_default_interceptor_chain(nullptr);
    }
    ~default_chain_scope() {
        crpc::obs::set_default_interceptor_chain(nullptr);
    }
    default_chain_scope(const default_chain_scope&) = delete;
    default_chain_scope& operator=(const default_chain_scope&) = delete;
};

// Records the `after_call` notification the default hook forwards.
struct after_call_recorder final : crpc::obs::interceptor {
    int calls = 0;
    crpc::rpc_code last_code = crpc::rpc_code::ok;
    std::string last_message;
    crpc::obs::hook_point last_point = crpc::obs::hook_point::count;

    bool on_hook(crpc::obs::hook_point point,
                 crpc::obs::call_context& context) override {
        ++calls;
        last_point = point;
        last_code = context.result;
        last_message = context.error_message;
        return true;
    }
};

// A backend whose connection accepts and never completes a write: the call
// stays in flight so it can be cancelled before `get()`.
struct stalled_connection final : crpc::connection {
    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept override {
        return open.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const noexcept override {
        return crpc::endpoint::inproc("stalled");
    }

private:
    struct noop_source final : crpc::io::source {
        std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };
    struct never_sink final : crpc::io::sink {
        void push(crpc::span<const std::byte>, crpc::io::completion*) noexcept override {}
    };
    noop_source source_;
    never_sink sink_;
    std::atomic<bool> open{true};
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

struct echo_service {
    int echo(const int& value) { return value + 1; }
};

crpc::status fail_body(void*, const crpc::call_envelope&, crpc::span<const std::byte>,
                       crpc::buffer&, crpc::rpc::call_context&) {
    return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "boom")};
}

} // namespace

CRPC_TEST(diagnostics_completion_hook_fires_exactly_once_per_notification) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    crpc::rpc_error error(crpc::rpc_code::deadline_exceeded, "slow");
    error.origin = crpc::failure_origin::timeout;
    error.attempts = 2;

    crpc::diagnostics::on_completion(error);
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK(probe.last_code == crpc::rpc_code::deadline_exceeded);
    CRPC_CHECK(probe.last_origin == crpc::failure_origin::timeout);
    CRPC_CHECK_EQ(2u, probe.last_attempts);

    // A second notification is a second event, and it still fires exactly once.
    crpc::diagnostics::on_completion(error);
    CRPC_CHECK_EQ(2, probe.calls);
}

CRPC_TEST(diagnostics_completion_hook_can_be_replaced_and_cleared) {
    completion_hook_scope scope;
    completion_probe first;
    completion_probe second;

    crpc::diagnostics::set_completion_hook(&record_completion, &first);
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::unavailable));
    CRPC_CHECK_EQ(1, first.calls);
    CRPC_CHECK_EQ(0, second.calls);

    crpc::diagnostics::set_completion_hook(&record_completion, &second);
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::overload));
    CRPC_CHECK_EQ(1, first.calls);
    CRPC_CHECK_EQ(1, second.calls);
    CRPC_CHECK(second.last_code == crpc::rpc_code::overload);

    crpc::diagnostics::set_completion_hook(nullptr, nullptr);
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::internal));
    CRPC_CHECK_EQ(1, first.calls);
    CRPC_CHECK_EQ(1, second.calls);
}

CRPC_TEST(diagnostics_completion_hook_runs_without_holding_a_lock) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&clear_hook_on_completion, &probe);

    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::ok));
    CRPC_CHECK_EQ(1, probe.calls);

    // The hook cleared itself; the next notification is a no-op.
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::ok));
    CRPC_CHECK_EQ(1, probe.calls);
}

CRPC_TEST(diagnostics_has_completion_observer_reports_hook_and_chain) {
    completion_hook_scope hook_scope;
    default_chain_scope chain_scope;
    completion_probe probe;

    CRPC_CHECK_FALSE(crpc::diagnostics::has_completion_observer());

    crpc::diagnostics::set_completion_hook(&record_completion, &probe);
    CRPC_CHECK(crpc::diagnostics::has_completion_observer());

    crpc::diagnostics::set_completion_hook(nullptr, nullptr);
    CRPC_CHECK_FALSE(crpc::diagnostics::has_completion_observer());

    // The default observability chain counts as an observer too, so
    // `notify_completion` still enriches when only the chain is installed.
    crpc::obs::interceptor_chain chain;
    after_call_recorder recorder;
    CRPC_CHECK(chain.add(crpc::obs::hook_point::after_call, &recorder));
    chain.freeze();
    crpc::obs::set_default_interceptor_chain(&chain);
    CRPC_CHECK(crpc::diagnostics::has_completion_observer());
}

CRPC_TEST(diagnostics_default_hook_triggers_installed_interceptor) {
    completion_hook_scope hook_scope;
    default_chain_scope chain_scope;

    crpc::obs::interceptor_chain chain;
    after_call_recorder recorder;
    CRPC_CHECK(chain.add(crpc::obs::hook_point::after_call, &recorder));
    chain.freeze();
    crpc::obs::set_default_interceptor_chain(&chain);

    crpc::rpc_error error(crpc::rpc_code::unavailable, "no endpoint");
    error.origin = crpc::failure_origin::local_capacity;
    crpc::diagnostics::on_completion(error);

    CRPC_CHECK_EQ(1, recorder.calls);
    CRPC_CHECK(recorder.last_point == crpc::obs::hook_point::after_call);
    CRPC_CHECK(recorder.last_code == crpc::rpc_code::unavailable);
    CRPC_CHECK(recorder.last_message == "no endpoint");

    // A success carries `ok` and an empty message.
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::ok));
    CRPC_CHECK_EQ(2, recorder.calls);
    CRPC_CHECK(recorder.last_code == crpc::rpc_code::ok);
    CRPC_CHECK(recorder.last_message.empty());
}

CRPC_TEST(diagnostics_default_hook_is_noop_without_interceptor) {
    completion_hook_scope hook_scope;
    default_chain_scope chain_scope;

    // No hook and no chain: the notification must be a safe no-op, and the
    // observer query must report false so `notify_completion` skips enrichment.
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::internal));
    CRPC_CHECK_FALSE(crpc::diagnostics::has_completion_observer());
}

CRPC_TEST(diagnostics_user_hook_takes_precedence_over_default_chain) {
    completion_hook_scope hook_scope;
    default_chain_scope chain_scope;
    completion_probe probe;

    crpc::obs::interceptor_chain chain;
    after_call_recorder recorder;
    CRPC_CHECK(chain.add(crpc::obs::hook_point::after_call, &recorder));
    chain.freeze();
    crpc::obs::set_default_interceptor_chain(&chain);

    crpc::diagnostics::set_completion_hook(&record_completion, &probe);
    crpc::diagnostics::on_completion(crpc::rpc_error(crpc::rpc_code::overload));

    CRPC_CHECK_EQ(1, probe.calls);
    // The user hook replaced the default: the chain is not also run.
    CRPC_CHECK_EQ(0, recorder.calls);
}

CRPC_TEST(diagnostics_response_get_notifies_computed_success_once) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    crpc::response<int> r(crpc::rpc::rpc_result<int>(42));

    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK(got.ok());
    CRPC_CHECK_EQ(42, got.value());
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK(probe.last_code == crpc::rpc_code::ok);

    // Reading the same terminal response again is not a new completion.
    const crpc::rpc::rpc_result<int> again = r.get();
    CRPC_CHECK(again.ok());
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK_EQ(42, r.value());
    CRPC_CHECK_EQ(1, probe.calls);
}

CRPC_TEST(diagnostics_response_get_notifies_start_failure_once) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    crpc::rpc_error error(crpc::rpc_code::unavailable, "no endpoint");
    error.origin = crpc::failure_origin::local_capacity;
    error.attempts = 4;
    crpc::response<int> r = crpc::response<int>::error(error);

    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK(got.error().code == crpc::rpc_code::unavailable);
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK(probe.last_code == crpc::rpc_code::unavailable);
    CRPC_CHECK(probe.last_origin == crpc::failure_origin::local_capacity);
    CRPC_CHECK_EQ(4u, probe.last_attempts);

    (void)r.get();
    CRPC_CHECK_EQ(1, probe.calls);
}

CRPC_TEST(diagnostics_response_get_notifies_after_try_get) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));

    crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
    CRPC_CHECK(r.try_get(out));
    CRPC_CHECK(out.error().code == crpc::rpc_code::unavailable);
    // `try_get` is the non-blocking read; the hook is bound to `get()`.
    CRPC_CHECK_EQ(0, probe.calls);

    // `try_get` cached the terminal result; the first `get()` still reports the
    // completion exactly once.
    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK(probe.last_code == crpc::rpc_code::unavailable);

    (void)r.get();
    CRPC_CHECK_EQ(1, probe.calls);
}

CRPC_TEST(diagnostics_response_get_notifies_started_success_once) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("diagnostics-completion-ok"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("diagnostics-completion-ok"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    crpc::response<int> r =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{1}, 41);

    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK(got.ok());
    CRPC_CHECK_EQ(42, got.value());
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK(probe.last_code == crpc::rpc_code::ok);
    // A success has no error result, but the hook's `ok` record still carries
    // the call's terminal origin/attempts for diagnostics.
    CRPC_CHECK(probe.last_origin == crpc::failure_origin::remote);
    CRPC_CHECK_EQ(r.attempts(), probe.last_attempts);

    (void)r.get();
    CRPC_CHECK_EQ(1, probe.calls);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

CRPC_TEST(diagnostics_response_get_notifies_started_failure_once) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    crpc::inproc_backend backend;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("diagnostics-completion-fail"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("diagnostics-completion-fail"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    crpc::response<int> r =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{2}, 0);

    // The non-blocking read carries the call's terminal diagnostics too, so a
    // later `get()` served from the cache is enriched as well. `try_get` is
    // non-blocking by contract, so poll it until the call completes.
    crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
    CRPC_CHECK(crpc_test::wait_for([&r, &out] { return r.try_get(out); }, 2000));
    CRPC_CHECK_FALSE(out.ok());
    CRPC_CHECK(out.error().origin == crpc::failure_origin::remote);
    CRPC_CHECK(out.error().attempts >= 1u);
    CRPC_CHECK_EQ(0, probe.calls);

    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK(got.error().code == crpc::rpc_code::unavailable);
    // `get()` returns the error with the call's terminal origin/attempts, not
    // the `rpc_error` defaults.
    CRPC_CHECK(got.error().origin == crpc::failure_origin::remote);
    CRPC_CHECK(got.error().attempts >= 1u);
    CRPC_CHECK_EQ(r.attempts(), got.error().attempts);
    CRPC_CHECK_EQ(1, probe.calls);
    CRPC_CHECK(probe.last_code == crpc::rpc_code::unavailable);
    // The hook observes the same diagnostics as the returned error.
    CRPC_CHECK(probe.last_origin == got.error().origin);
    CRPC_CHECK_EQ(got.error().attempts, probe.last_attempts);

    (void)r.get();
    CRPC_CHECK_EQ(1, probe.calls);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

CRPC_TEST(diagnostics_response_get_notifies_cancel_once) {
    completion_hook_scope scope;
    completion_probe probe;
    crpc::diagnostics::set_completion_hook(&record_completion, &probe);

    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("diagnostics-completion-cancel"), backend);
    cb.pool_size(1);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
            std::move(started).value());
        crpc::response<int> r(std::move(wrapped));

        r.cancel();
        const crpc::rpc::rpc_result<int> got = r.get();
        CRPC_CHECK_FALSE(got.ok());
        CRPC_CHECK(got.error().code == crpc::rpc_code::canceled);
        CRPC_CHECK(got.error().origin == crpc::failure_origin::cancel);
        CRPC_CHECK_EQ(1, probe.calls);
        CRPC_CHECK(probe.last_code == crpc::rpc_code::canceled);
        CRPC_CHECK(probe.last_origin == crpc::failure_origin::cancel);
        CRPC_CHECK_EQ(got.error().attempts, probe.last_attempts);

        (void)r.get();
        CRPC_CHECK_EQ(1, probe.calls);
    }

    cli->close();
    shard.stop();
    shard.join();
}

// --- Task 4: trace metadata --------------------------------------------------
//
// A started call carries the caller's W3C trace context. When the call fails,
// `response<Resp>::get()` attaches the trace id to the terminal `rpc_error`
// metadata under the `trace_id` key, so a caller can correlate the failure with
// the trace that produced it without re-deriving the context. A call with no
// trace id (the all-zero default) must not invent one: the key stays absent.

namespace {

// The 16 bytes 0x01..0x10, and their lowercase hexadecimal form.
constexpr std::byte k_trace_bytes[16] = {
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
    std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
    std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
    std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10},
};
constexpr const char* k_trace_hex = "0102030405060708090a0b0c0d0e0f10";

} // namespace

CRPC_TEST(diagnostics_response_error_metadata_carries_trace_id) {
    crpc::inproc_backend backend;
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("diagnostics-trace-id"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("diagnostics-trace-id"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    crpc::rpc::call_context ctx;
    for (std::size_t i = 0; i < ctx.trace.trace.bytes.size(); ++i) {
        ctx.trace.trace.bytes[i] = k_trace_bytes[i];
    }

    crpc::response<int> traced =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{2}, 0, ctx);
    const crpc::rpc::rpc_result<int> failed = traced.get();
    CRPC_CHECK_FALSE(failed.ok());
    const std::string* trace = failed.error().metadata.find("trace_id");
    CRPC_CHECK(trace != nullptr);
    if (trace != nullptr) {
        CRPC_CHECK_EQ(std::string(k_trace_hex), *trace);
    }

    // A call with the default (invalid) trace id must not fabricate metadata.
    crpc::response<int> untraced =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{2}, 0);
    const crpc::rpc::rpc_result<int> plain = untraced.get();
    CRPC_CHECK_FALSE(plain.ok());
    CRPC_CHECK(plain.error().metadata.find("trace_id") == nullptr);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
