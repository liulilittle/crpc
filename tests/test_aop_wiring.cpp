// CRPC -- AOP interceptor chain wiring tests (requirement 17).
//
// These cases pin the contract of the `obs::interceptor_chain` outlets wired
// into the server call path (`server::builder::interceptor_chain`):
//
//   * Pinned order (flat pipeline, NOT a nested onion): per request the
//     sequence is legacy `server_interceptor::on_request` (registration
//     order) -> chain `before_call` (registration order) -> admission ->
//     handler -> chain `after_call` (registration order) -> legacy
//     `server_interceptor::on_response` with the final outcome. Both hook
//     points iterate in registration order; there is no reverse/unwind phase.
//   * Short-circuit: a `before_call` hook returning false skips admission,
//     the handler and `after_call`; the client receives the chain verdict
//     (`call_context::result`, defaulted to `canceled` by the chain when the
//     hook set none).
//   * Exception boundary: a hook that throws never crosses the server
//     boundary; the chain converts it to `rpc_code::internal` and the
//     connection stays usable.
//   * Context flow: one `obs::call_context` is shared by both hook points of
//     one request, and its `metadata` is injected into the handler-visible
//     `rpc::call_context` through the dispatcher's context-injection overload.
//
// The client facade deliberately gets no new chain: the terminal outcome is
// already observed through `obs::set_default_interceptor_chain` at
// `after_call` by `diagnostics::on_completion` (see docs/architecture.md).
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/io.hpp>
#include <crpc/observability/interceptor.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace obs = crpc::obs;

namespace {

constexpr crpc::u64 kService = 700;
constexpr crpc::u64 kEcho = 1;
constexpr crpc::u64 kContextEcho = 2;

// Shared event log for order assertions. The server may run hooks on
// different handler threads, so appends are serialized.
struct shared_log {
    std::mutex mu;
    std::vector<std::string> events;

    void push(std::string event) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return events;
    }
};

// Scriptable interceptor: counts hook invocations, appends to a shared log,
// and can be told to refuse (with or without a verdict) or to throw at either
// hook point. Flags are atomic because a test disables them from another
// thread while the server's handler threads may read them.
class scripted final : public obs::interceptor {
public:
    scripted(std::string name, std::shared_ptr<shared_log> log)
        : name_(std::move(name)), log_(std::move(log)) {}

    scripted& throw_before(bool value) {
        throw_before_.store(value, std::memory_order_relaxed);
        return *this;
    }
    scripted& throw_after(bool value) {
        throw_after_.store(value, std::memory_order_relaxed);
        return *this;
    }
    scripted& refuse(bool value) {
        refuse_.store(value, std::memory_order_relaxed);
        return *this;
    }
    scripted& verdict(crpc::rpc_code code, const char* message) {
        verdict_.store(static_cast<int>(code), std::memory_order_relaxed);
        message_ = message;
        return *this;
    }

    bool on_hook(obs::hook_point point, obs::call_context& context) override {
        if (point == obs::hook_point::before_call) {
            before_calls.fetch_add(1, std::memory_order_relaxed);
            if (log_ != nullptr) {
                log_->push(name_ + ":before");
            }
            if (throw_before_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("before hook exploded");
            }
            if (refuse_.load(std::memory_order_relaxed)) {
                const auto code = static_cast<crpc::rpc_code>(
                    verdict_.load(std::memory_order_relaxed));
                if (code != crpc::rpc_code::ok) {
                    context.set_error(code, message_);
                }
                return false;
            }
            return true;
        }
        if (point == obs::hook_point::after_call) {
            after_calls.fetch_add(1, std::memory_order_relaxed);
            if (log_ != nullptr) {
                log_->push(name_ + ":after");
            }
            seen_result.store(static_cast<int>(context.result),
                               std::memory_order_relaxed);
            if (throw_after_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("after hook exploded");
            }
            return true;
        }
        return true;
    }

    std::atomic<int> before_calls{0};
    std::atomic<int> after_calls{0};
    // The dispatched outcome pre-filled by the server before `after_call` runs.
    std::atomic<int> seen_result{0};

private:
    std::string name_;
    std::shared_ptr<shared_log> log_;
    std::atomic<bool> throw_before_{false};
    std::atomic<bool> throw_after_{false};
    std::atomic<bool> refuse_{false};
    std::atomic<int> verdict_{static_cast<int>(crpc::rpc_code::ok)};
    const char* message_ = "";
};

// Bare counter used by the concurrency case (atomics only, no log).
class counter final : public obs::interceptor {
public:
    std::atomic<int> before{0};
    std::atomic<int> after{0};

    bool on_hook(obs::hook_point point, obs::call_context&) override {
        if (point == obs::hook_point::before_call) {
            before.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (point == obs::hook_point::after_call) {
            after.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return true;
    }
};

// Writes into the shared per-call context and records what the response-side
// hook can still see.
class metadata_writer final : public obs::interceptor {
public:
    std::atomic<int> before_calls{0};
    std::atomic<int> after_calls{0};
    std::atomic<bool> after_saw_service{false};
    std::atomic<bool> after_saw_method{false};
    std::atomic<bool> after_saw_k1{false};
    std::atomic<bool> after_result_ok{false};

    bool on_hook(obs::hook_point point, obs::call_context& context) override {
        if (point == obs::hook_point::before_call) {
            before_calls.fetch_add(1, std::memory_order_relaxed);
            context.metadata.set("k1", "v1");
            context.priority = 7;
            return true;
        }
        if (point == obs::hook_point::after_call) {
            after_calls.fetch_add(1, std::memory_order_relaxed);
            after_saw_service.store(context.service == "svc",
                                    std::memory_order_relaxed);
            after_saw_method.store(context.method == "ctx_echo",
                                   std::memory_order_relaxed);
            const std::string* k1 = context.metadata.find("k1");
            after_saw_k1.store(k1 != nullptr && *k1 == "v1",
                               std::memory_order_relaxed);
            after_result_ok.store(context.result == crpc::rpc_code::ok,
                                  std::memory_order_relaxed);
            return true;
        }
        return true;
    }
};

// Reads what an earlier interceptor wrote, then adds its own entry.
class metadata_reader final : public obs::interceptor {
public:
    std::atomic<int> before_calls{0};
    std::atomic<bool> saw_k1{false};

    bool on_hook(obs::hook_point point, obs::call_context& context) override {
        if (point == obs::hook_point::before_call) {
            before_calls.fetch_add(1, std::memory_order_relaxed);
            const std::string* k1 = context.metadata.find("k1");
            saw_k1.store(k1 != nullptr && *k1 == "v1",
                         std::memory_order_relaxed);
            context.metadata.set("k2", "v2");
            return true;
        }
        return true;
    }
};

// The pre-existing `server_interceptor` observer, unchanged by this feature.
struct legacy_probe final : crpc::server_interceptor {
    std::atomic<int> requests{0};
    std::atomic<int> responses{0};
    std::shared_ptr<shared_log> log;
    // Atomic: `on_response` runs on the shard thread and the case reads this
    // from the main thread once the call has returned. The call returning only
    // means the response was written, not that `on_response` has finished with
    // it, so a plain member is a genuine data race (ThreadSanitizer reports
    // exactly this pair). The other two counters were already atomic.
    std::atomic<crpc::rpc_code> last_code{crpc::rpc_code::ok};

    void on_request(const crpc::frame&) noexcept override {
        requests.fetch_add(1, std::memory_order_relaxed);
        if (log != nullptr) {
            log->push("legacy:request");
        }
    }
    void on_response(crpc::rpc_code code) noexcept override {
        responses.fetch_add(1, std::memory_order_relaxed);
        last_code.store(code, std::memory_order_release);
        if (log != nullptr) {
            log->push("legacy:response:" + std::string(crpc::to_string(code)));
        }
    }
};

struct aop_service {
    std::atomic<int> echo_calls{0};
    std::atomic<int> ctx_calls{0};

    int echo(const int& value) {
        echo_calls.fetch_add(1, std::memory_order_relaxed);
        return value + 1;
    }

    // The handler sees interceptor-written metadata through the dispatcher's
    // context-injection overload and echoes it back as the response.
    std::string ctx_echo(const std::string& value, crpc::rpc::call_context& ctx) {
        ctx_calls.fetch_add(1, std::memory_order_relaxed);
        std::string out = value;
        const std::string* k1 = ctx.metadata.find(std::string("k1"));
        if (k1 != nullptr) {
            out += ";k1=" + *k1;
        }
        const std::string* k2 = ctx.metadata.find(std::string("k2"));
        if (k2 != nullptr) {
            out += ";k2=" + *k2;
        }
        return out;
    }
};

// A coroutine body plus a completion flag (same pattern as
// tests/test_server_client.cpp).
struct coroutine_job {
    std::function<void()> body;
    std::atomic<bool> done{false};
};

void coroutine_entry(void* ctx) {
    std::shared_ptr<coroutine_job> job(
        std::move(*static_cast<std::shared_ptr<coroutine_job>*>(ctx)));
    delete static_cast<std::shared_ptr<coroutine_job>*>(ctx);
    job->body();
    job->done.store(true, std::memory_order_release);
}

bool run_on_shard(crpc::runtime& shard, std::function<void()> body,
                  int timeout_ms = 10000) {
    auto job = std::make_shared<coroutine_job>();
    job->body = std::move(body);
    auto* ctx = new std::shared_ptr<coroutine_job>(job);
    if (!shard.spawn(&coroutine_entry, ctx)) {
        delete ctx;
        return false;
    }
    return crpc_test::wait_for(
        [job] { return job->done.load(std::memory_order_acquire); }, timeout_ms);
}

crpc::resilience::breaker_config relaxed_breaker() {
    // Pin the slow threshold high so ambient latency can never open a breaker
    // under load (same rule as tests/test_server_client.cpp).
    crpc::resilience::breaker_config config;
    config.slow_call_threshold = 5s;
    return config;
}

struct harness {
    crpc::inproc_backend backend;
    aop_service service;
    std::unique_ptr<crpc::runtime> shard;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    const std::string name = "crpc-aop-wiring";

    ~harness() {
        if (cli != nullptr) {
            cli->close();
        }
        if (srv != nullptr) {
            srv->stop();
        }
        if (shard != nullptr) {
            shard->stop();
            shard->join();
        }
    }
};

bool start_harness(harness& h, obs::interceptor_chain* chain,
                   crpc::server_interceptor* legacy) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(h.name), h.backend);
    sb.add_service(kService, "svc");
    sb.add_method<aop_service, &aop_service::echo>(kService, kEcho, "echo",
                                                   h.service);
    sb.add_method<aop_service, &aop_service::ctx_echo>(kService, kContextEcho,
                                                       "ctx_echo", h.service);
    if (legacy != nullptr) {
        sb.add_interceptor(*legacy);
    }
    if (chain != nullptr) {
        sb.interceptor_chain(*chain);
    }
    sb.shards(1);
    sb.handler_workers(4);
    sb.drain_timeout(2000ms);
    sb.breaker(relaxed_breaker());
    h.srv = sb.build();
    if (h.srv == nullptr || !h.srv->start().ok()) {
        return false;
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    h.shard = std::make_unique<crpc::runtime>(ro);
    h.shard->start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(h.name), h.backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.breaker(relaxed_breaker());
    cb.on_shard(*h.shard);
    h.cli = cb.build();
    return h.cli != nullptr;
}

crpc::rpc::rpc_result<int> call_echo(crpc::client& cli, int value) {
    auto stub = cli.service<int, int>(kService, kEcho);
    auto started = stub.call(value);
    if (!started.ok()) {
        return crpc::rpc::rpc_result<int>(started.error());
    }
    return std::move(started).value().get();
}

crpc::rpc::rpc_result<std::string> call_ctx_echo(crpc::client& cli,
                                                 const std::string& value) {
    auto stub = cli.service<std::string, std::string>(kService, kContextEcho);
    auto started = stub.call(value);
    if (!started.ok()) {
        return crpc::rpc::rpc_result<std::string>(started.error());
    }
    return std::move(started).value().get();
}

} // namespace

// --- 1. no chain: the shipped behaviour is unchanged -------------------------

CRPC_TEST(no_chain_baseline_unchanged) {
    harness h;
    CRPC_CHECK(start_harness(h, nullptr, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto first = call_echo(*h.cli, 1);
        CRPC_CHECK(first.ok());
        CRPC_CHECK_EQ(2, first.value());
        auto second = call_echo(*h.cli, 41);
        CRPC_CHECK(second.ok());
        CRPC_CHECK_EQ(42, second.value());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(2, h.service.echo_calls.load());
}

// --- 2. the legacy observer keeps firing with no chain installed -------------

CRPC_TEST(legacy_server_interceptor_still_fires) {
    // Declared before `h`: the harness destructor stops the server, and the
    // server calls `on_response` on this object right up to that point. If
    // `probe` were declared after `h`, it would be destroyed first and the
    // shard thread could still be inside `on_response` when it went away.
    legacy_probe probe;
    harness h;
    CRPC_CHECK(start_harness(h, nullptr, &probe));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_echo(*h.cli, 1);
        CRPC_CHECK(result.ok());
        CRPC_CHECK_EQ(2, result.value());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(1, probe.requests.load());
    CRPC_CHECK_EQ(1, probe.responses.load());
    CRPC_CHECK(probe.last_code.load(std::memory_order_acquire) == crpc::rpc_code::ok);
}

// --- 3. pinned flat order: legacy -> before_call -> after_call -> legacy -----

CRPC_TEST(hook_order_is_pinned_flat_registration_order) {
    // The documented order (see the file comment): legacy on_request, then
    // before_call in registration order, then the handler, then after_call in
    // registration order, then legacy on_response with the final outcome. No
    // reverse/unwind phase exists at either hook point.
    // Same lifetime rule as the test above: everything the server calls
    // must outlive the harness that stops it.
    auto log = std::make_shared<shared_log>();
    scripted a("a", log);
    scripted b("b", log);
    legacy_probe probe;
    probe.log = log;
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &a));
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &b));
    CRPC_CHECK(chain.add(obs::hook_point::after_call, &a));
    CRPC_CHECK(chain.add(obs::hook_point::after_call, &b));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, &probe));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_echo(*h.cli, 1);
        CRPC_CHECK(result.ok());
    });
    CRPC_CHECK(ran);
    const std::vector<std::string> expected = {
        "legacy:request", "a:before", "b:before",
        "a:after",        "b:after",  "legacy:response:ok"};
    const std::vector<std::string> actual = log->snapshot();
    CRPC_CHECK_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size() && i < actual.size(); ++i) {
        CRPC_CHECK_EQ(expected[i], actual[i]);
    }
}

// --- 4. short-circuit: the handler never runs, the verdict is the response ---

CRPC_TEST(short_circuit_skips_handler_and_after_call) {
    // Verdict codes ride the response codec, which fails closed on codes above
    // `application` (the client turns raw 22 into `internal` plus a
    // `crpc.raw_code` metadata entry), so the pinned case uses a code that
    // round-trips: `failed_precondition`.
    scripted gate("gate", nullptr);
    gate.refuse(true).verdict(crpc::rpc_code::failed_precondition,
                              "rejected-by-aop");
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &gate));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto refused = call_echo(*h.cli, 1);
        CRPC_CHECK_FALSE(refused.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(refused.code()));
        CRPC_CHECK_EQ(std::string("rejected-by-aop"), refused.error().message);
        CRPC_CHECK_EQ(0, h.service.echo_calls.load());
        CRPC_CHECK_EQ(0, gate.after_calls.load());
        // Once the interceptor lets calls through, the same connection works.
        gate.refuse(false);
        auto allowed = call_echo(*h.cli, 5);
        CRPC_CHECK(allowed.ok());
        CRPC_CHECK_EQ(6, allowed.value());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(2, gate.before_calls.load());
    CRPC_CHECK_EQ(1, h.service.echo_calls.load());
}

// --- 5. short-circuit without a verdict: the chain defaults to canceled ------

CRPC_TEST(short_circuit_default_verdict_is_canceled) {
    scripted gate("gate", nullptr);
    gate.refuse(true);
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &gate));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto refused = call_echo(*h.cli, 1);
        CRPC_CHECK_FALSE(refused.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::canceled),
                      static_cast<int>(refused.code()));
        CRPC_CHECK_EQ(std::string("interceptor short-circuited"),
                      refused.error().message);
        CRPC_CHECK_EQ(0, h.service.echo_calls.load());
    });
    CRPC_CHECK(ran);
}

// --- 6. a throwing before_call hook becomes internal; the server survives ----

CRPC_TEST(throwing_before_call_becomes_internal_and_survives) {
    scripted bomb("bomb", nullptr);
    bomb.throw_before(true);
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &bomb));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_echo(*h.cli, 1);
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                      static_cast<int>(result.code()));
        CRPC_CHECK_EQ(std::string("interceptor threw"), result.error().message);
        CRPC_CHECK_EQ(0, h.service.echo_calls.load());
        // The connection and the server are still usable afterwards.
        bomb.throw_before(false);
        auto next = call_echo(*h.cli, 10);
        CRPC_CHECK(next.ok());
        CRPC_CHECK_EQ(11, next.value());
    });
    CRPC_CHECK(ran);
}

// --- 7. a throwing after_call hook vetoes the response with internal ---------

CRPC_TEST(throwing_after_call_becomes_internal_and_survives) {
    scripted bomb("bomb", nullptr);
    bomb.throw_after(true);
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &bomb));
    CRPC_CHECK(chain.add(obs::hook_point::after_call, &bomb));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_echo(*h.cli, 1);
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                      static_cast<int>(result.code()));
        CRPC_CHECK_EQ(std::string("interceptor threw"), result.error().message);
        // The handler DID run; the response-side veto only replaced the answer.
        CRPC_CHECK_EQ(1, h.service.echo_calls.load());
        bomb.throw_after(false);
        auto next = call_echo(*h.cli, 10);
        CRPC_CHECK(next.ok());
        CRPC_CHECK_EQ(11, next.value());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(2, h.service.echo_calls.load());
}

// --- 8. context flows: interceptor -> interceptor -> handler -> after_call ---

CRPC_TEST(context_flows_between_hooks_and_handler) {
    metadata_writer writer;
    metadata_reader reader;
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &writer));
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &reader));
    CRPC_CHECK(chain.add(obs::hook_point::after_call, &writer));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_ctx_echo(*h.cli, "ctx");
        CRPC_CHECK(result.ok());
        CRPC_CHECK_EQ(std::string("ctx;k1=v1;k2=v2"), result.value());
    });
    CRPC_CHECK(ran);
    // The second before_call interceptor saw the first one's entry.
    CRPC_CHECK(reader.saw_k1.load());
    // The response-side hook shares the same per-call context.
    CRPC_CHECK(writer.after_saw_service.load());
    CRPC_CHECK(writer.after_saw_method.load());
    CRPC_CHECK(writer.after_saw_k1.load());
    CRPC_CHECK(writer.after_result_ok.load());
    CRPC_CHECK_EQ(1, writer.before_calls.load());
    CRPC_CHECK_EQ(1, reader.before_calls.load());
    CRPC_CHECK_EQ(1, writer.after_calls.load());
}

// --- 9. a response-side-only chain runs once per dispatched call -------------

CRPC_TEST(after_call_only_chain_observes_dispatched_outcome) {
    scripted observer("observer", nullptr);
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::after_call, &observer));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_echo(*h.cli, 1);
        CRPC_CHECK(result.ok());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(0, observer.before_calls.load());
    CRPC_CHECK_EQ(1, observer.after_calls.load());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok), observer.seen_result.load());
    CRPC_CHECK_EQ(1, h.service.echo_calls.load());
}

// --- 10. eight interceptors keep registration order at both hook points ------

CRPC_TEST(eight_interceptors_keep_registration_order) {
    auto log = std::make_shared<shared_log>();
    std::vector<std::unique_ptr<scripted>> hooks;
    obs::interceptor_chain chain;
    for (std::size_t i = 0;
         i < obs::interceptor_chain::max_interceptors_per_hook; ++i) {
        hooks.push_back(std::make_unique<scripted>("h" + std::to_string(i), log));
        CRPC_CHECK(chain.add(obs::hook_point::before_call, hooks.back().get()));
        CRPC_CHECK(chain.add(obs::hook_point::after_call, hooks.back().get()));
    }
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto result = call_echo(*h.cli, 1);
        CRPC_CHECK(result.ok());
    });
    CRPC_CHECK(ran);
    const std::vector<std::string> actual = log->snapshot();
    const std::size_t total =
        2 * obs::interceptor_chain::max_interceptors_per_hook;
    CRPC_CHECK_EQ(total, actual.size());
    for (std::size_t i = 0; i < total && i < actual.size(); ++i) {
        const std::size_t hook_index = i % hooks.size();
        const char* phase = i < hooks.size() ? ":before" : ":after";
        CRPC_CHECK_EQ("h" + std::to_string(hook_index) + phase, actual[i]);
    }
}

// --- 11. concurrent calls: the frozen chain is safe and exact ----------------

CRPC_TEST(concurrent_calls_are_all_intercepted) {
    // One server, four concurrent clients on four threads. The chain runs on
    // the server's handler threads, so this exercises concurrent `run()` on a
    // frozen chain. Failures are tallied in atomics: the framework counters
    // are not shared-thread-safe, so assertions run after the join.
    constexpr int kThreads = 4;
    constexpr int kCallsPerThread = 25;
    counter tally;
    obs::interceptor_chain chain;
    CRPC_CHECK(chain.add(obs::hook_point::before_call, &tally));
    CRPC_CHECK(chain.add(obs::hook_point::after_call, &tally));
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));

    std::atomic<int> ok_calls{0};
    std::atomic<int> bad_calls{0};
    std::atomic<int> start_failures{0};
    // Shards and clients are built here, on the test thread, and only the
    // calls run concurrently. Building them inside the worker threads put four
    // simultaneous `runtime` constructions into corocc's driver registry, whose
    // hand-rolled spinlock (a CAS on a `uintptr_t` in
    // `third_party/corocc/src/scheduler/rt_driver.c`) ThreadSanitizer cannot see
    // -- it reported the registry walk as racing with another thread's
    // `dup_cstr`. That is a limitation of the analysis, not a defect this test
    // is about: the subject here is `interceptor_chain::run` under concurrency,
    // and concurrent runtime construction is corocc's own concern with its own
    // tests. Keeping construction serial preserves the intent and keeps the
    // TSan gate meaningful.
    struct worker {
        std::unique_ptr<crpc::runtime> shard;
        std::unique_ptr<crpc::client> cli;
    };
    std::vector<worker> workers;
    for (int t = 0; t < kThreads; ++t) {
        crpc::runtime_options ro;
        ro.start_handler_pool = false;
        worker w;
        w.shard = std::make_unique<crpc::runtime>(ro);
        w.shard->start_thread();
        crpc::client::builder cb;
        cb.add_endpoint(crpc::endpoint::inproc(h.name), h.backend);
        cb.pool_size(1);
        cb.default_deadline(5s);
        cb.breaker(relaxed_breaker());
        cb.on_shard(*w.shard);
        w.cli = cb.build();
        if (w.cli == nullptr) {
            start_failures.fetch_add(1, std::memory_order_relaxed);
        }
        workers.push_back(std::move(w));
    }
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                if (workers[static_cast<std::size_t>(t)].cli == nullptr) {
                    return;
                }
                const bool ran = run_on_shard(
                    *workers[static_cast<std::size_t>(t)].shard, [&, t] {
                        for (int i = 0; i < kCallsPerThread; ++i) {
                            auto result =
                                call_echo(*workers[static_cast<std::size_t>(t)].cli, i);
                            if (result.ok() && result.value() == i + 1) {
                                ok_calls.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                bad_calls.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    });
                if (!ran) {
                    start_failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (std::thread& th : threads) {
            th.join();
        }
    }
    for (worker& w : workers) {
        w.cli->close();
        w.shard->stop();
        w.shard->join();
    }
    CRPC_CHECK_EQ(0, start_failures.load());
    CRPC_CHECK_EQ(0, bad_calls.load());
    CRPC_CHECK_EQ(kThreads * kCallsPerThread, ok_calls.load());
    CRPC_CHECK_EQ(kThreads * kCallsPerThread, tally.before.load());
    CRPC_CHECK_EQ(kThreads * kCallsPerThread, tally.after.load());
    CRPC_CHECK_EQ(kThreads * kCallsPerThread, h.service.echo_calls.load());
}

// --- 12. an installed but empty chain freezes and changes nothing ------------

CRPC_TEST(empty_chain_freezes_in_start_and_changes_nothing) {
    obs::interceptor_chain chain;
    CRPC_CHECK_FALSE(chain.frozen());
    harness h;
    CRPC_CHECK(start_harness(h, &chain, nullptr));
    // The server froze the chain before accepting (its add()s are now dead).
    CRPC_CHECK(chain.frozen());
    CRPC_CHECK_FALSE(chain.add(obs::hook_point::before_call, nullptr));
    const bool ran = run_on_shard(*h.shard, [&] {
        auto first = call_echo(*h.cli, 1);
        CRPC_CHECK(first.ok());
        CRPC_CHECK_EQ(2, first.value());
        auto second = call_echo(*h.cli, 7);
        CRPC_CHECK(second.ok());
        CRPC_CHECK_EQ(8, second.value());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(2, h.service.echo_calls.load());
}

CRPC_TEST_MAIN()
