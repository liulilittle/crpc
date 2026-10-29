// CRPC -- admission vocabulary: the single table from a rejection stage to the
// rpc_code that goes on the wire.
//
// This header is deliberately tiny and header-only: no exported symbols, no
// layout change, no behaviour. It exists so the two ends cannot disagree about
// which code a stage answers.
//
// Each reference below pairs a semantic anchor (function, variable or
// condition) with its line number in this tree. The anchor is authoritative:
// a later edit that moves a line does not invalidate the contract, only the
// number.
//
// CONTRACT -- read before adding a rejection branch
// ------------------------------------------------
// 1. Every rejection branch MUST obtain its rpc_code from reject_code().
//    A literal rpc_code::overload in a new branch is a review defect: the
//    single table is the only place the wire contract is allowed to live.
//    Branches this slice did not convert still carry their literal; they are
//    listed in stage_kind so the next slice can convert them without inventing
//    a second vocabulary:
//      - the per-connection flood bucket (server.cpp submit(), `if (rate)`,
//        1426) -- see connection_rate;
//      - the per-connection in-flight cap (server.cpp submit(),
//        `per_connection_inflight`, 1451);
//      - the client connection pool capacity (client_facade.cpp acquire_slot,
//        `"connection pool is saturated"` / `"connection pool is exhausted"`,
//        1122/1128). It HAS a stage name (pool_capacity, this file, 128), so it
//        is "named but not converted", unlike the entry below, which has no
//        stage at all;
//      - `cannot track endpoint candidates` (client_facade.cpp begin_attempt,
//        `tried.reset(pool_count)`, 829-839): PENDING, no stage yet.
//        It is an allocation failure, not an admission decision, and it is
//        deliberately not retried.
// 2. The rate stages are the ONLY stages whose code depends on the deadline.
//    reject_code(stage_kind::service_rate, deadline_met=false) ==
//    deadline_exceeded; with deadline_met=true it is resource_exhausted. The
//    client's endpoint_rate is identical. Both are asserted. Every other stage
//    explicitly discards the parameter with `(void)deadline_met;`, so a future
//    stage cannot silently inherit rate semantics.
// 3. A probe does NOT bypass every stage. It bypasses per-endpoint /
//    per-service rate and bulkhead, but the per-service in-flight cap still
//    applies (server.cpp handle_request(), `if (!enter_service_cap(*guard))`,
//    1293, sits outside the `if (opt.resilience_enabled && !probe)`
//    guard at 1215). Never write "probes skip admission".
// 4. Refactoring a rejection branch is NOT a no-op. Four fields drift
//    silently if the branch is collapsed:
//      a. rpc_error::retryable -- send_error() sets it from is_retryable()
//         (server.cpp, `error.retryable = is_retryable(code)`, 1065).
//      b. rpc_error::retry_after -- the bulkhead falls back to
//         overload_retry_after when the permit carries 0 (server.cpp,
//         `bulkhead_permit.retry_after().count() > 0
//              ? bulkhead_permit.retry_after() : opt.overload_retry_after`,
//         1275-1277).
//      c. the per-stage statistics counter -- rate_limited_service,
//         deadline_rejected, bulkhead_rejected, rejected and breaker_open are
//         five DIFFERENT counters with different meanings (server.cpp,
//         1240,1242,1272,1295,1191).
//      d. the error message text -- seven distinct strings (server.cpp,
//         "rate limit exceeded"/"overloaded" in the forced-error path,
//         1106-1107; "service circuit breaker is open":1196;
//         "service rate limit cannot meet the deadline":1249;
//         "service rate limit reached":1257; "service bulkhead is full":1281;
//         "service concurrency limit reached":1301).
//    S1 acceptance therefore compares code + retryable + retry_after +
//    message + counter delta, field by field.
// 5. A one-way request has no response frame (server.cpp handle_request(),
//    `wants_response = job.header.type != frame_type::one_way`, 934;
//    every send_error() call site is inside `if (wants_response)`). It can
//    never be degraded and can never carry crpc-degradable, so
//    fallback_eligible() rejects `one_way` unconditionally.
// 6. Four admission refusals DO write the reserved `crpc-degradable: true`
//    response meta entry, and only when the request still has at least
//    min_remaining_ns of wire deadline left:
//      - the per-service breaker (`!permit.allowed()`, send_error at 1217);
//      - the per-service rate bucket, success exit (send_error at 1277);
//      - the per-service bulkhead (send_error at 1302);
//      - the per-service in-flight cap (send_error at 1321).
//    The rate bucket's OTHER exit -- deadline not met (send_error at 1266) --
//    never sets it: a deadline miss is not degradable (fallback_eligible()
//    rejects deadline_exceeded first). The connection-level flood bucket and
//    the per-connection cap (both reach send_error() through the forced-error
//    job) never set it either: they are load shedding with no eligible stage,
//    and a fallback response would cost exactly what the shed exists to avoid.
//    `crpc-degraded` (the client-side "a fallback value was used" key) is still
//    set by nobody on the server. `fallback_execute` is still never called from
//    the server: the marker advertises that a refusal COULD be degraded, it does
//    not perform the degradation.
//    This is why fallback_eligible() cannot be used to derive the marker from
//    the code alone: `overload` from a handler is not marked, `overload` from
//    the bulkhead or the cap is. Only the branch knows.
//
// fallback_eligible() answers only "may this (code, stage) pair be degraded?".
// The idempotency gate for a path that may already have been sent is the
// caller's job, not this function's: it cannot see whether the request left
// the process. A caller on a sent path MUST also require an idempotent call.
// This table covers the admission stages; transport failures are added by the
// slice that consumes them.
#ifndef CRPC_RESILIENCE_ADMISSION_HPP
#define CRPC_RESILIENCE_ADMISSION_HPP

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc::resilience {

// The minimum remaining time, in nanoseconds, for the server to start work on a
// request. A wire deadline below this floor is already unusable: the dispatcher
// refuses it before the handler runs (dispatcher.cpp, "deadline already
// passed") and the server's admission refusals withhold the crpc-degradable
// marker for it, because a fallback value could not be delivered before the
// caller's deadline either. Both readers share this constant; a second literal
// would be a second deadline policy.
//
// This is the single authority for the server's admission floor. The client has
// no equivalent floor on purpose: `client.cpp` and `client_facade.cpp` only ask
// whether the call has already expired locally, which is a different question.
// A future client-side "give up early" floor MUST reuse this constant rather
// than add a second 1'000'000 or 1ms literal. `fallback_policy::min_remaining`
// is a different, caller-configured floor ("is the degraded value still
// deliverable"); `set_fallback` defaults it to this constant, never to a
// literal. See docs/abi.md section 4.
inline constexpr u64 min_remaining_ns = 1'000'000;

// Where a request was refused. The names follow the real branches in
// src/server/server.cpp and src/client/client_facade.cpp, not an abstract
// pipeline: a stage is only listed if some code actually refuses there.
enum class stage_kind : u8 {
    // Per-connection token bucket, before any job allocation. Load shedding:
    // the bucket exists to avoid the work of a full request, so answering with
    // a fallback value would defeat it (server.cpp submit(), `if (rate)`,
    // 1614).
    connection_rate,
    // Per-service token bucket. Two outcomes, see reject_code()
    // (server.cpp handle_request(), `if (guard->rate)`, 1216).
    service_rate,
    // Per-service bounded concurrency (server.cpp handle_request(),
    // `if (guard->bulkhead)`, 1268).
    service_bulkhead,
    // Per-service in-flight cap. Applies to probes too (server.cpp
    // handle_request(), `if (!enter_service_cap(*guard))`, 1293).
    service_capacity,
    // Per-endpoint token bucket on the client (client_facade.cpp
    // begin_attempt(), `if (pool.rate.has_value())`, 950).
    // Two outcomes, exactly like service_rate.
    endpoint_rate,
    // Per-endpoint bounded concurrency on the client (client_facade.cpp
    // begin_attempt(), `if (pool.bulkhead.has_value())`, 980).
    endpoint_bulkhead,
    // Endpoint / service circuit breaker (server.cpp
    // `permit = guard->breaker->allow()`, 1022; client_facade.cpp
    // `pools[index]->breaker.allow()`, 875).
    breaker,
    // Client-local: no usable endpoint was selected. This is a PRE-SEND
    // failure; the request never left the process (client_facade.cpp
    // `failure = rpc_error(..., "no usable endpoint")`, 913, and
    // `"no endpoint configured"`, 812).
    dispatch,
    // The caller's deadline elapsed before the attempt started
    // (client_facade.cpp `"deadline already passed"`, 799).
    deadline,
    // Client-local capacity: the connection pool is saturated or exhausted
    // (client_facade.cpp `"connection pool is saturated"` / `"exhausted"`,
    // 1122,1128). Named after the resource, not after
    // `client_detail::failure_origin::local_gate`, which also covers the
    // client rate and bulkhead rejections and therefore means something else.
    pool_capacity
};

// The three admission outcomes. RESERVED: no stage consumes this type yet. S1
// and S2 must not change any control flow based on it -- probe semantics are
// already expressed structurally by the `!probe` guard (server.cpp,
// 1048) and by the position of the per-service cap outside it
// (1126). The type exists only so a future slice does not have to
// invent a second vocabulary; `probe_bypass` means "the half-open probe is
// allowed past THIS stage", never a global pass.
enum class admission_decision : u8 { admit, reject, probe_bypass };

// Maps a rejection to the wire contract. One table, shared by both sides.
//
// `deadline_met` exists because the rate stages have TWO outcomes from the same
// branch: a bucket that cannot meet the caller's deadline answers
// `deadline_exceeded` (retry_after 0, "waiting would not help"), while a bucket
// that is merely empty answers `resource_exhausted` (retry_after = hint).
// Every other stage ignores the parameter, so the default keeps existing call
// sites unchanged.
//
// A new rate call site MUST pass `deadline_met` explicitly. The default of
// `true` is a convenience for non-rate stages only; relying on it for a rate
// branch silently answers `resource_exhausted` where the remaining time would
// require `deadline_exceeded`.
[[nodiscard]] constexpr rpc_code reject_code(stage_kind kind,
                                             bool deadline_met = true) noexcept {
    switch (kind) {
    case stage_kind::service_rate:
    case stage_kind::endpoint_rate:
        return deadline_met ? rpc_code::resource_exhausted
                            : rpc_code::deadline_exceeded;
    case stage_kind::connection_rate:
    case stage_kind::pool_capacity:
        (void)deadline_met;
        return rpc_code::resource_exhausted;
    case stage_kind::service_bulkhead:
    case stage_kind::service_capacity:
    case stage_kind::endpoint_bulkhead:
        (void)deadline_met;
        return rpc_code::overload;
    case stage_kind::breaker:
    case stage_kind::dispatch:
        (void)deadline_met;
        return rpc_code::unavailable;
    case stage_kind::deadline:
        (void)deadline_met;
        return rpc_code::deadline_exceeded;
    }
    (void)deadline_met;
    return rpc_code::internal;
}

// True when the (code, stage) pair may be answered by a fallback value.
//
// `one_way` is a call shape, not a failure: a one-way call has no response
// frame, so it can never be degraded.
[[nodiscard]] constexpr bool fallback_eligible(rpc_code code, stage_kind stage,
                                               bool one_way) noexcept {
    if (one_way) {
        return false;
    }
    // A deadline miss is never degraded: returning a value after the caller's
    // deadline would turn a timeout into a fake success.
    if (code == rpc_code::deadline_exceeded) {
        return false;
    }
    switch (stage) {
    case stage_kind::breaker:
    case stage_kind::dispatch:
        return code == rpc_code::unavailable;
    case stage_kind::service_bulkhead:
    case stage_kind::endpoint_bulkhead:
    case stage_kind::service_capacity:
        // `service_capacity` answers `overload` and nothing else; the only
        // producer is the per-service cap (server.cpp, 1126).
        return code == rpc_code::overload;
    case stage_kind::service_rate:
    case stage_kind::endpoint_rate:
    case stage_kind::pool_capacity:
        // `pool_capacity` is client-local: degrading there adds no load to the
        // server, so it stays eligible unlike `connection_rate`.
        return code == rpc_code::resource_exhausted;
    case stage_kind::connection_rate:
        // Load shedding: the bucket rejects before any job is assigned, and a
        // fallback would have to build, encode and write a response -- exactly
        // the cost the bucket is meant to avoid.
        return false;
    case stage_kind::deadline:
        return false;
    }
    return false;
}

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_ADMISSION_HPP
