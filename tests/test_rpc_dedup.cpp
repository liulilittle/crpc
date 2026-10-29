// CRPC -- server-side idempotency dedup table tests.
//
// Requirement 12 (reliable/unreliable delivery semantics) and requirement 16
// (retry budget): the server must recognize a replayed `call_id` and answer it
// without running the handler twice. Every boundary the architecture documents
// is pinned here:
//
//   1. exactly once: the same call_id replays the cached response;
//   2. concurrent duplicates: two copies arriving at the same time execute the
//      handler exactly once and both receive the response;
//   3. distinct call ids are independent;
//   4. a request without a key (or with an all-zero key) behaves as before;
//   5. an expired/evicted entry re-executes (the documented window limit);
//   6. a response larger than the cache bound is not cached, but the in-flight
//      window still deduplicates;
//   7. a suspended duplicate ends with an explicit error when its connection
//      closes (dispatcher-level abandon and the server-level wiring);
//   8. the table stays inside its capacity bound;
//   plus: failed calls are not replayed, one-way duplicates are dropped, a
//   table full of in-flight entries degrades to bypass, and the end-to-end
//   server path replays a cached response for two distinct clients.
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/runtime.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

using crpc::buffer;
using crpc::call_envelope;
using crpc::codec_id;
using crpc::encode_call_envelope;
using crpc::encoded_call_envelope_size;
using crpc::rpc_code;
using crpc::rpc_error;
using crpc::u64;
using crpc::rpc::call_context;
using crpc::rpc::call_id;
using crpc::rpc::decode_response;
using crpc::rpc::dedup_options;
using crpc::rpc::dedup_statistics;
using crpc::rpc::dispatcher;
using crpc::rpc::method_options;
using crpc::rpc::service_registry;
using crpc::status;
using crpc::trace_extension;

namespace crpc {
inline bool operator==(const span<const std::byte>& a, const span<const std::byte>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
} // namespace crpc

namespace {

// --- keyed request builder ----------------------------------------------------

call_id make_key(unsigned char seed) {
    call_id id;
    for (std::size_t i = 0; i < id.client_id.size(); ++i) {
        id.client_id[i] = static_cast<std::byte>(seed + static_cast<unsigned char>(i));
    }
    id.client_epoch = 7;
    id.seq = seed;
    return id;
}

struct built_request {
    buffer payload;
    crpc::frame value;
};

// Encodes a REQUEST (or ONE_WAY) frame whose envelope carries the 32-byte key.
built_request make_call(u64 service_id, u64 method_id, const call_id& key, bool with_key,
                        bool one_way = false) {
    call_envelope envelope;
    envelope.service_id = service_id;
    envelope.method_id = method_id;
    envelope.codec = codec_id::crpc_binary;
    const std::byte body = std::byte{0x2A};
    envelope.body = crpc::span<const std::byte>(&body, 1);
    const std::array<std::byte, 32> encoded = key.encode();
    if (with_key) {
        envelope.call_flags = crpc::call_flag_has_extensions;
        envelope.extensions.flags = trace_extension::has_idempotency_key;
        envelope.extensions.idempotency_key =
            crpc::span<const std::byte>(encoded.data(), encoded.size());
    }

    built_request request;
    const std::size_t size = encoded_call_envelope_size(envelope);
    (void)request.payload.reserve(size);
    std::size_t written = 0;
    (void)encode_call_envelope(
        crpc::span<std::byte>(request.payload.data(), size), envelope, written);
    request.value.header.type = one_way ? crpc::frame_type::one_way : crpc::frame_type::request;
    request.value.header.stream_id = 1;
    request.value.payload = crpc::span<const std::byte>(request.payload.data(), written);
    return request;
}

// --- shared handler state -----------------------------------------------------

struct handler_state {
    std::atomic<int> calls{0};
    // Sleep inside the handler to widen the in-flight window.
    int sleep_ms = 0;
    // Fail the first N invocations with `unavailable`.
    std::atomic<int> failures_remaining{0};
    // Response body size (echoes the request body when zero).
    std::size_t response_bytes = 0;
};

status counting_handler(void* self, const call_envelope&, crpc::span<const std::byte> body,
                        buffer& response, call_context&) {
    auto* state = static_cast<handler_state*>(self);
    state->calls.fetch_add(1, std::memory_order_acq_rel);
    if (state->sleep_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(state->sleep_ms));
    }
    if (state->failures_remaining.fetch_sub(1, std::memory_order_acq_rel) > 0) {
        return status{rpc_error(rpc_code::unavailable, "flaky failure")};
    }
    if (state->response_bytes > 0) {
        const std::vector<std::byte> filler(state->response_bytes, std::byte{0x5A});
        return response.append(filler.data(), filler.size()) ? status{}
                                                             : status{rpc_code::encode_error};
    }
    return response.append(body) ? status{} : status{rpc_code::encode_error};
}

struct fixture {
    service_registry registry;
    dedup_options options;
    handler_state state;
    dispatcher dispatch{registry, options};

    explicit fixture(dedup_options opts) : options(std::move(opts)) {
        (void)registry.register_service(1, "svc");
        (void)registry.register_method(1, 1, "op", &counting_handler, &state,
                                       method_options{});
        (void)registry.freeze();
    }
};

struct dispatch_result {
    status dispatched;
    buffer response;
    bool has_response = false;
    bool handler_ran = false;
};

dispatch_result run(dispatcher& d, const built_request& request, const void* wait_key = nullptr) {
    dispatch_result out;
    out.dispatched = d.dispatch(request.value, out.response, out.has_response,
                                std::chrono::steady_clock::now(), out.handler_ran, wait_key);
    return out;
}

rpc_code response_code(const dispatch_result& result) {
    if (!result.dispatched.ok()) {
        return result.dispatched.code();
    }
    if (!result.has_response) {
        return rpc_code::ok;
    }
    rpc_code code = rpc_code::ok;
    rpc_error error;
    crpc::span<const std::byte> body;
    if (!decode_response(result.response.readable(), code, error, body).ok()) {
        return rpc_code::protocol_error;
    }
    return code;
}

} // namespace

// 1. Exactly once: the same call_id three times -> one execution, three
// identical responses.
CRPC_TEST(dedup_same_call_id_executes_once_and_replays) {
    fixture fx{dedup_options{}};
    const built_request request = make_call(1, 1, make_key(1), true);

    dispatch_result first = run(fx.dispatch, request);
    CRPC_CHECK(first.dispatched.ok());
    CRPC_CHECK(first.has_response);
    CRPC_CHECK_EQ(1, fx.state.calls.load());

    dispatch_result second = run(fx.dispatch, request);
    dispatch_result third = run(fx.dispatch, request);
    CRPC_CHECK(second.dispatched.ok());
    CRPC_CHECK(third.dispatched.ok());
    CRPC_CHECK_EQ(1, fx.state.calls.load());
    CRPC_CHECK(second.response.readable() == first.response.readable());
    CRPC_CHECK(third.response.readable() == first.response.readable());

    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK_EQ(1u, stats.claims_leader);
    CRPC_CHECK_EQ(2u, stats.claims_replay);
    // A replay does not report the handler as run.
    CRPC_CHECK(first.handler_ran);
    CRPC_CHECK_FALSE(second.handler_ran);
}

// 2. Concurrent duplicates: both arrive while the handler is running; the
// handler executes exactly once and both callers receive the response.
CRPC_TEST(dedup_concurrent_duplicates_execute_once) {
    dedup_options options;
    options.max_wait = 2000ms;
    fixture fx{options};
    fx.state.sleep_ms = 150;
    const built_request request = make_call(1, 1, make_key(2), true);

    std::atomic<bool> first_ok{false};
    std::atomic<bool> second_ok{false};
    buffer first_response;
    buffer second_response;
    auto leader = std::thread([&] {
        dispatch_result result = run(fx.dispatch, request);
        first_ok.store(result.dispatched.ok() && result.has_response, std::memory_order_release);
        first_response = std::move(result.response);
    });
    auto follower = std::thread([&] {
        // Give the leader a moment to claim the entry first.
        std::this_thread::sleep_for(20ms);
        dispatch_result result = run(fx.dispatch, request);
        second_ok.store(result.dispatched.ok() && result.has_response, std::memory_order_release);
        second_response = std::move(result.response);
    });
    leader.join();
    follower.join();

    CRPC_CHECK(first_ok.load());
    CRPC_CHECK(second_ok.load());
    CRPC_CHECK_EQ(1, fx.state.calls.load());
    CRPC_CHECK(second_response.readable() == first_response.readable());

    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK_EQ(1u, stats.claims_leader);
    CRPC_CHECK_EQ(1u, stats.claims_waiter);
    CRPC_CHECK_EQ(1u, stats.waiters_woken);
}

// 3. Distinct call ids are independent: N keys, N executions.
CRPC_TEST(dedup_different_call_ids_are_independent) {
    fixture fx{dedup_options{}};
    for (unsigned char seed = 1; seed <= 5; ++seed) {
        const built_request request = make_call(1, 1, make_key(seed), true);
        const dispatch_result result = run(fx.dispatch, request);
        CRPC_CHECK(result.dispatched.ok());
    }
    CRPC_CHECK_EQ(5, fx.state.calls.load());
}

// 4. Regression: a request without a key -- or with an all-zero key -- behaves
// exactly as before the table existed.
CRPC_TEST(dedup_without_key_behaves_as_before) {
    fixture fx{dedup_options{}};
    for (int i = 0; i < 3; ++i) {
        const built_request request = make_call(1, 1, call_id{}, false);
        const dispatch_result result = run(fx.dispatch, request);
        CRPC_CHECK(result.dispatched.ok());
    }
    for (int i = 0; i < 3; ++i) {
        // The zero key is the "not set" marker even when the flag is present.
        const built_request request = make_call(1, 1, call_id{}, true);
        const dispatch_result result = run(fx.dispatch, request);
        CRPC_CHECK(result.dispatched.ok());
    }
    CRPC_CHECK_EQ(6, fx.state.calls.load());
    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK_EQ(0u, stats.claims_leader);
    CRPC_CHECK_EQ(0u, stats.claims_replay);
}

// 5. The documented window limit: after the TTL the entry no longer
// deduplicates and the handler runs again.
CRPC_TEST(dedup_expired_entry_reruns_handler) {
    dedup_options options;
    options.entry_ttl = 50ms;
    fixture fx{options};
    const built_request request = make_call(1, 1, make_key(3), true);

    dispatch_result first = run(fx.dispatch, request);
    CRPC_CHECK(first.dispatched.ok());
    CRPC_CHECK_EQ(1, fx.state.calls.load());

    std::this_thread::sleep_for(120ms);
    dispatch_result second = run(fx.dispatch, request);
    CRPC_CHECK(second.dispatched.ok());
    CRPC_CHECK_EQ(2, fx.state.calls.load());
    CRPC_CHECK(second.response.readable() == first.response.readable());
}

// 6. An oversized response is not cached, but the in-flight window still
// deduplicates: concurrent duplicates execute once; a later replay re-executes.
CRPC_TEST(dedup_oversized_response_not_cached_but_still_deduped) {
    dedup_options options;
    options.max_wait = 2000ms;
    options.max_cached_bytes = 8;
    fixture fx{options};
    fx.state.response_bytes = 64;
    // Long enough that a thread created after the leader claimed still
    // reaches dispatch inside the window. 100 ms was a bet on thread
    // start-up latency, and on a loaded x86 Debug machine it lost: the
    // leader finished first, the follower re-executed, and `calls` became
    // 2 instead of 1.
    fx.state.sleep_ms = 500;
    const built_request request = make_call(1, 1, make_key(4), true);

    std::atomic<bool> first_ok{false};
    std::atomic<bool> second_ok{false};
    buffer first_response;
    buffer second_response;
    const auto body_of = [](const dispatch_result& result) {
        rpc_code code = rpc_code::ok;
        rpc_error error;
        crpc::span<const std::byte> body;
        if (!result.has_response ||
            !decode_response(result.response.readable(), code, error, body).ok()) {
            return crpc::span<const std::byte>();
        }
        return body;
    };
    auto leader = std::thread([&] {
        dispatch_result result = run(fx.dispatch, request);
        first_ok.store(result.dispatched.ok() && body_of(result).size() == 64,
                       std::memory_order_release);
        first_response = std::move(result.response);
    });
    // The follower must arrive while the leader is still in flight. Poll the
    // table's own record of the claim instead of sleeping a fixed 20 ms.
    const auto claim_deadline = std::chrono::steady_clock::now() + 5s;
    while (fx.dispatch.dedup().stats().claims_leader == 0 &&
           std::chrono::steady_clock::now() < claim_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CRPC_CHECK_EQ(1u, fx.dispatch.dedup().stats().claims_leader);
    auto follower = std::thread([&] {
        dispatch_result result = run(fx.dispatch, request);
        second_ok.store(result.dispatched.ok() && body_of(result).size() == 64,
                        std::memory_order_release);
        second_response = std::move(result.response);
    });
    leader.join();
    follower.join();
    // The dedup path, not the re-execute path, must have served the second
    // call: that is what `calls == 1` below means, and pinning it directly
    // makes a failure say which of the two went wrong.
    CRPC_CHECK_EQ(1u, fx.dispatch.dedup().stats().claims_waiter);

    // The in-flight window deduplicated: one execution, both served.
    CRPC_CHECK(first_ok.load());
    CRPC_CHECK(second_ok.load());
    CRPC_CHECK_EQ(1, fx.state.calls.load());
    CRPC_CHECK(second_response.readable() == first_response.readable());

    // The oversized response was not cached: a later replay re-executes.
    dispatch_result third = run(fx.dispatch, request);
    CRPC_CHECK(third.dispatched.ok());
    CRPC_CHECK_EQ(2, fx.state.calls.load());
    CRPC_CHECK(body_of(third).size() == 64);
}

// 7. A suspended duplicate ends with an explicit error when its connection
// closes (`abandon`), and the leader is unaffected.
CRPC_TEST(dedup_abandoned_waiter_gets_explicit_error) {
    dedup_options options;
    options.max_wait = 5000ms;
    fixture fx{options};
    fx.state.sleep_ms = 300;
    const built_request request = make_call(1, 1, make_key(5), true);

    int connection = 0;
    std::atomic<bool> leader_ok{false};
    std::atomic<bool> follower_abandoned{false};
    std::atomic<int> follower_calls{0};
    auto leader = std::thread([&] {
        dispatch_result result = run(fx.dispatch, request);
        leader_ok.store(result.dispatched.ok() && result.has_response, std::memory_order_release);
    });
    // Wait for the leader to own the key instead of sleeping: a fixed delay is
    // a bet on thread start-up latency, and on a loaded x86 Debug machine the
    // follower could claim first (then it would be the leader, and the abandon
    // below would find no waiter).
    const auto claim_deadline = std::chrono::steady_clock::now() + 5s;
    while (fx.dispatch.dedup().stats().claims_leader == 0 &&
           std::chrono::steady_clock::now() < claim_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CRPC_CHECK_EQ(1u, fx.dispatch.dedup().stats().claims_leader);

    auto follower = std::thread([&] {
        dispatch_result result = run(fx.dispatch, request, &connection);
        follower_calls.store(fx.state.calls.load(), std::memory_order_release);
        follower_abandoned.store(result.dispatched.ok() &&
                                     response_code(result) == rpc_code::canceled,
                                 std::memory_order_release);
    });

    // The follower must actually be suspended before its connection is closed;
    // `claims_waiter` is the table's own record of that transition, so poll it
    // rather than guessing how long a thread needs to get there.
    const auto suspend_deadline = std::chrono::steady_clock::now() + 5s;
    while (fx.dispatch.dedup().stats().claims_waiter == 0 &&
           std::chrono::steady_clock::now() < suspend_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CRPC_CHECK_EQ(1u, fx.dispatch.dedup().stats().claims_waiter);

    const std::size_t ended = fx.dispatch.dedup().abandon(&connection);
    CRPC_CHECK_EQ(1u, ended);

    leader.join();
    follower.join();
    CRPC_CHECK(leader_ok.load());
    CRPC_CHECK(follower_abandoned.load());
    // The follower never ran the handler; the leader ran it once.
    CRPC_CHECK_EQ(1, follower_calls.load());
    CRPC_CHECK_EQ(1, fx.state.calls.load());

    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK_EQ(1u, stats.waiters_abandoned);
}

// 8. The table never grows beyond its capacity bound; the least recently used
// entry yields and a replay of an evicted key re-executes.
CRPC_TEST(dedup_capacity_is_bounded) {
    dedup_options options;
    options.capacity = 3;
    fixture fx{options};

    for (unsigned char seed = 1; seed <= 6; ++seed) {
        const built_request request = make_call(1, 1, make_key(seed), true);
        const dispatch_result result = run(fx.dispatch, request);
        CRPC_CHECK(result.dispatched.ok());
    }
    CRPC_CHECK_EQ(6, fx.state.calls.load());

    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK(stats.entries <= 3);
    CRPC_CHECK(stats.evictions >= 3);

    // Key #1 was evicted: replaying it re-executes the handler.
    const built_request replay = make_call(1, 1, make_key(1), true);
    const dispatch_result result = run(fx.dispatch, replay);
    CRPC_CHECK(result.dispatched.ok());
    CRPC_CHECK_EQ(7, fx.state.calls.load());
}

// 9. A failed call is never replayed: the entry completes as an uncached
// tombstone and the retry runs the handler again.
CRPC_TEST(dedup_failed_call_is_not_replayed) {
    fixture fx{dedup_options{}};
    fx.state.failures_remaining.store(1);
    const built_request request = make_call(1, 1, make_key(6), true);

    dispatch_result first = run(fx.dispatch, request);
    CRPC_CHECK(first.dispatched.ok());
    CRPC_CHECK_EQ(rpc_code::unavailable, response_code(first));
    CRPC_CHECK_EQ(1, fx.state.calls.load());

    dispatch_result second = run(fx.dispatch, request);
    CRPC_CHECK(second.dispatched.ok());
    CRPC_CHECK_EQ(rpc_code::ok, response_code(second));
    CRPC_CHECK_EQ(2, fx.state.calls.load());
}

// 10. A replayed ONE_WAY duplicate is dropped silently; the side effect ran
// once and nobody waits for a response that will never come.
CRPC_TEST(dedup_one_way_duplicate_executes_once) {
    fixture fx{dedup_options{}};
    const built_request request = make_call(1, 1, make_key(7), true, /*one_way=*/true);

    dispatch_result first = run(fx.dispatch, request);
    CRPC_CHECK(first.dispatched.ok());
    CRPC_CHECK_FALSE(first.has_response);
    CRPC_CHECK_EQ(1, fx.state.calls.load());

    dispatch_result second = run(fx.dispatch, request);
    CRPC_CHECK(second.dispatched.ok());
    CRPC_CHECK_FALSE(second.has_response);
    CRPC_CHECK_EQ(1, fx.state.calls.load());

    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK_EQ(1u, stats.claims_leader);
    CRPC_CHECK_EQ(0u, stats.claims_waiter);
}

// 11. A table saturated with in-flight entries degrades to bypass: the new key
// executes without dedup instead of hanging or growing memory.
CRPC_TEST(dedup_table_full_of_inflight_bypasses) {
    dedup_options options;
    options.capacity = 1;
    options.max_wait = 2000ms;
    fixture fx{options};
    fx.state.sleep_ms = 150;
    const built_request leader_request = make_call(1, 1, make_key(8), true);

    std::atomic<bool> leader_ok{false};
    auto leader = std::thread([&] {
        dispatch_result result = run(fx.dispatch, leader_request);
        leader_ok.store(result.dispatched.ok() && result.has_response, std::memory_order_release);
    });
    // Deterministic claim ordering: wait until the leader's handler is inside
    // its sleep, so its entry is in flight when the second key arrives.
    CRPC_CHECK(crpc_test::wait_for([&fx] { return fx.state.calls.load() >= 1; }, 2000));

    const built_request other = make_call(1, 1, make_key(9), true);
    const dispatch_result result = run(fx.dispatch, other);
    CRPC_CHECK(result.dispatched.ok());
    CRPC_CHECK(result.has_response);
    // The leader's handler plus the bypass execution.
    CRPC_CHECK_EQ(2, fx.state.calls.load());

    leader.join();
    CRPC_CHECK(leader_ok.load());
    const dedup_statistics stats = fx.dispatch.dedup().stats();
    CRPC_CHECK_EQ(1u, stats.claims_bypass);
}

// --- server wiring ------------------------------------------------------------

struct gate_state {
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};
};

status gated_handler(void* self, const call_envelope&, crpc::span<const std::byte> body,
                     buffer& response, call_context&) {
    auto* state = static_cast<gate_state*>(self);
    state->calls.fetch_add(1, std::memory_order_acq_rel);
    state->entered.store(true, std::memory_order_release);
    while (!state->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return response.append(body) ? status{} : status{rpc_code::encode_error};
}

// 12. End to end: two clients presenting the same call_id to a real server;
// the second receives the cached response without a second execution.
CRPC_TEST(server_dedup_replays_cached_response) {
    crpc::inproc_backend backend;
    gate_state state;
    // The gate stays open here: no in-flight window is needed for the
    // sequential replay path (test 13 exercises the gated one).
    state.release.store(true, std::memory_order_release);

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("dedup-replay"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 1, "op", &gated_handler, &state);
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

    auto make_client = [&](const char* name) {
        crpc::client::builder cb;
        cb.add_endpoint(crpc::endpoint::inproc(name), backend);
        cb.pool_size(1);
        cb.default_deadline(5s);
        cb.on_shard(shard);
        return cb.build();
    };
    auto first = make_client("dedup-replay");
    auto second = make_client("dedup-replay");
    if (!first || !second) {
        if (first) {
            first->close();
        }
        if (second) {
            second->close();
        }
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    // The same explicit R6 triple on both calls: this is what a client that
    // reuses its key across a reconnect looks like on the wire.
    const call_id key = make_key(10);
    const auto call_once = [&](crpc::client& client) {
        auto stub = client.service<std::string, std::string>(1, 1);
        crpc::rpc::call_context ctx;
        ctx.idempotent = true;
        ctx.has_idempotency_key = true;
        ctx.idempotency = key;
        auto started = stub.call(std::string("payload"), ctx);
        if (!started.ok()) {
            return crpc::rpc::rpc_result<std::string>(rpc_error(started.code(), "start failed"));
        }
        return started.value().get();
    };

    const crpc::rpc::rpc_result<std::string> first_result = call_once(*first);
    CRPC_CHECK(first_result.ok());
    CRPC_CHECK_EQ(1, state.calls.load());

    const crpc::rpc::rpc_result<std::string> second_result = call_once(*second);
    CRPC_CHECK(second_result.ok());
    if (first_result.ok() && second_result.ok()) {
        CRPC_CHECK_EQ(first_result.value(), second_result.value());
    }
    CRPC_CHECK_EQ(1, state.calls.load());

    first->close();
    second->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// 13. Server wiring: a duplicate suspended on shard B while the original runs
// on shard A ends with an explicit error when its connection closes.
CRPC_TEST(server_dedup_abandons_suspended_duplicate_on_disconnect) {
    crpc::inproc_backend backend;
    gate_state state;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("dedup-abandon"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 1, "op", &gated_handler, &state);
    sb.shards(2);
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

    auto make_client = [&](const char* name) {
        crpc::client::builder cb;
        cb.add_endpoint(crpc::endpoint::inproc(name), backend);
        cb.pool_size(1);
        cb.default_deadline(5s);
        cb.on_shard(shard);
        return cb.build();
    };
    auto leader_client = make_client("dedup-abandon");
    auto follower_client = make_client("dedup-abandon");
    if (!leader_client || !follower_client) {
        if (leader_client) {
            leader_client->close();
        }
        if (follower_client) {
            follower_client->close();
        }
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    const call_id key = make_key(11);
    const auto call_once = [&](crpc::client& client) {
        auto stub = client.service<std::string, std::string>(1, 1);
        crpc::rpc::call_context ctx;
        ctx.idempotent = true;
        ctx.has_idempotency_key = true;
        ctx.idempotency = key;
        auto started = stub.call(std::string("payload"), ctx);
        if (!started.ok()) {
            return crpc::rpc::rpc_result<std::string>(rpc_error(started.code(), "start failed"));
        }
        return started.value().get();
    };

    std::atomic<bool> leader_done{false};
    crpc::rpc::rpc_result<std::string> leader_result(
        rpc_error(rpc_code::canceled, "leader call did not run"));
    auto leader = std::thread([&] {
        leader_result = call_once(*leader_client);
        leader_done.store(true, std::memory_order_release);
    });
    CRPC_CHECK(crpc_test::wait_for([&state] { return state.entered.load(); }, 3000));

    std::atomic<bool> follower_done{false};
    auto follower = std::thread([&] {
        (void)call_once(*follower_client);
        follower_done.store(true, std::memory_order_release);
    });
    // Wait until the server holds both calls, then close the follower's
    // connection. A flat 200 ms sleep was a bet on how long the follower
    // takes to reach dispatch, and on a loaded machine it lost: the close
    // arrived before the request was even handled, so `requests_handled`
    // was 1 instead of 2. `in_flight_calls` is the server's own record that
    // both jobs were admitted.
    CRPC_CHECK(crpc_test::wait_for(
        [&srv] { return srv->stats().in_flight_calls >= 2; }, 5000));
    follower_client->close();

    state.release.store(true, std::memory_order_release);
    CRPC_CHECK(crpc_test::wait_for([&] { return leader_done.load() && follower_done.load(); },
                                   5000));

    leader.join();
    follower.join();
    CRPC_CHECK(leader_result.ok());
    CRPC_CHECK_EQ(1, state.calls.load());
    // Both requests were handled (one executed, one abandoned) and the server
    // is still healthy afterwards.
    CRPC_CHECK_EQ(2u, srv->stats().requests_handled);

    leader_client->close();
    srv->stop();
    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
