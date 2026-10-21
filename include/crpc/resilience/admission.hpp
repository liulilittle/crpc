// CRPC -- admission vocabulary: the single table from a rejection stage to the
// rpc_code that goes on the wire.
//
// This header is deliberately tiny and header-only: no exported symbols, no
// layout change, no behaviour. It exists so the two ends cannot disagree about
// which code a stage answers.
//
// CONTRACT -- read before adding a rejection branch
// ------------------------------------------------
// 1. Every rejection branch MUST obtain its rpc_code from reject_code().
//    A literal rpc_code::overload in a new branch is a review defect: the
//    single table is the only place the wire contract is allowed to live.
//    Branches this slice did not convert (the connection-level flood bucket,
//    the per-connection in-flight cap and the client connection-pool capacity
//    gate) still carry their literal; they are listed in stage_kind so the
//    next slice can convert them without inventing a second vocabulary.
// 2. The rate stages are the ONLY stages whose code depends on the deadline.
//    reject_code(stage_kind::service_rate, deadline_met=false) ==
//    deadline_exceeded; with deadline_met=true it is resource_exhausted. The
//    client's endpoint_rate is identical. Both are asserted. Every other stage
//    explicitly discards the parameter with `(void)deadline_met;`, so a future
//    stage cannot silently inherit rate semantics.
// 3. A probe does NOT bypass every stage. It bypasses per-endpoint /
//    per-service rate and bulkhead, but the per-service in-flight cap still
//    applies (src/server/server.cpp:1054 sits outside the `!probe` guard).
//    Never write "probes skip admission".
// 4. Refactoring a rejection branch is NOT a no-op. Four fields drift
//    silently if the branch is collapsed:
//      a. rpc_error::retryable  (server.cpp:873 sets it from is_retryable)
//      b. rpc_error::retry_after (bulkhead falls back to overload_retry_after
//         when the permit carries 0 -- server.cpp:1042-1045)
//      c. the per-stage statistics counter (rate_limited_service,
//         deadline_rejected, bulkhead_rejected, rejected, breaker_open are
//         five DIFFERENT counters -- server.cpp:1017,1019,1040,1056,974)
//      d. the error message text (seven distinct strings --
//         server.cpp:889,890,977,1024,1030,1047,1060)
//    S1 acceptance therefore compares code + retryable + retry_after +
//    message + counter delta, field by field.
// 5. A one-way request has no response frame. It can never be degraded and
//    can never carry crpc-degradable (server.cpp:884,1133-1138), so
//    fallback_eligible() rejects `one_way` unconditionally.
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

// Where a request was refused. The names follow the real branches in
// src/server/server.cpp and src/client/client_facade.cpp, not an abstract
// pipeline: a stage is only listed if some code actually refuses there.
enum class stage_kind : u8 {
    // Per-connection token bucket, before any job allocation
    // (server.cpp:1347).
    connection_rate,
    // Per-service token bucket. Two outcomes, see reject_code().
    service_rate,
    // Per-service bounded concurrency (server.cpp:1036).
    service_bulkhead,
    // Per-service in-flight cap. Applies to probes too (server.cpp:1054).
    service_capacity,
    // Per-endpoint token bucket on the client (client_facade.cpp:611).
    // Two outcomes, exactly like service_rate.
    endpoint_rate,
    // Per-endpoint bounded concurrency on the client
    // (client_facade.cpp:638).
    endpoint_bulkhead,
    // Endpoint / service circuit breaker (server.cpp:971,
    // client_facade.cpp:538).
    breaker,
    // The remote dispatch itself could not be reached.
    dispatch,
    // The caller's deadline elapsed.
    deadline,
    // A client-local capacity gate that is not a token bucket: the
    // connection pool is saturated or exhausted (client_facade.cpp:775-786).
    // Not converted in this slice; the name exists so the conversion does not
    // have to invent a second vocabulary.
    local_gate
};

// The three admission outcomes. `probe_bypass` means "the half-open probe is
// allowed past THIS stage"; it is not a global pass (see contract item 3).
enum class admission_decision : u8 { admit, reject, probe_bypass };

// Maps a rejection to the wire contract. One table, shared by both sides.
//
// `deadline_met` exists because the rate stage has TWO outcomes from the same
// branch: a bucket that cannot meet the caller's deadline answers
// `deadline_exceeded` (retry_after 0, "waiting would not help"), while a bucket
// that is merely empty answers `resource_exhausted` (retry_after = hint).
// Every other stage ignores the parameter, so the default keeps existing call
// sites unchanged.
[[nodiscard]] constexpr rpc_code reject_code(stage_kind kind,
                                             bool deadline_met = true) noexcept {
    switch (kind) {
    case stage_kind::service_rate:
    case stage_kind::endpoint_rate:
        return deadline_met ? rpc_code::resource_exhausted
                            : rpc_code::deadline_exceeded;
    case stage_kind::connection_rate:
    case stage_kind::local_gate:
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
        return code == rpc_code::overload;
    case stage_kind::service_capacity:
        // The per-service cap answers `overload` today; `resource_exhausted`
        // is admitted because the client-side capacity gate (pool saturated /
        // exhausted) uses it and both mean "no capacity right now".
        return code == rpc_code::overload ||
               code == rpc_code::resource_exhausted;
    case stage_kind::service_rate:
    case stage_kind::endpoint_rate:
    case stage_kind::connection_rate:
    case stage_kind::local_gate:
        return code == rpc_code::resource_exhausted;
    case stage_kind::deadline:
        return false;
    }
    return false;
}

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_ADMISSION_HPP
