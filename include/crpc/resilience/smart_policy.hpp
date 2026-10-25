// CRPC -- zero-config smart resilience defaults.
//
// `make_smart_policy` derives a complete resilience policy (timeout, retry,
// breaker, rate limit, bulkhead) from a small set of call traits, so a caller
// that configures nothing still gets a sane, self-consistent policy instead of
// the per-primitive defaults that were chosen independently.
//
// Design ruling (Ruling 18): `call_traits` is a new `resilience` value type
// rather than a reuse of `rpc::call_context`. `call_context` carries an
// absolute `steady_clock` deadline plus trace, metadata, lane and idempotency
// key fields that a policy derivation does not need, and it has no latency
// SLO. A dedicated trait keeps the derivation input minimal and lets a caller
// state "how long should this logical call be allowed to take" as a relative
// duration, which is what the derivation rules consume.
//
// The derivation rules are the ones in the design and the task brief:
//
//   * timeout  -- an explicit `deadline` wins, otherwise `default_smart_timeout`;
//   * retry    -- idempotent -> 3 attempts, otherwise 1; `initial_backoff` is
//                 timeout/10 and `max_backoff` is timeout/2;
//   * breaker  -- window 10s, `p99_target` timeout*2, `slow_call_threshold`
//                 timeout, `open_duration` 5s;
//   * rate     -- `default_smart_endpoint_rate_per_second`, burst = rate/10;
//   * bulkhead -- `default_smart_endpoint_concurrency`.
//
// The endpoint rate and concurrency are module-level defaults because they are
// properties of the dependency, not of a single call: `call_traits` is a
// per-call input and the derivation rules in this task take no endpoint
// argument. A caller that knows its endpoint capacity overrides the returned
// `rate`/`bulkhead` fields after the call (the "可覆盖" requirement).
//
// Thread safety: `call_traits` and `smart_policy` are plain value types; copy
// them before sharing between threads.
#ifndef CRPC_RESILIENCE_SMART_POLICY_HPP
#define CRPC_RESILIENCE_SMART_POLICY_HPP

#include <chrono>

#include <crpc/config.hpp>
#include <crpc/resilience/bulkhead.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/rate_limiter.hpp>
#include <crpc/resilience/retry.hpp>

namespace crpc::resilience {

// The timeout used when `call_traits::deadline` is not set explicitly.
inline constexpr std::chrono::nanoseconds default_smart_timeout{
    std::chrono::seconds(1)};

// The sustained request rate assumed for one endpoint, in requests per second.
// Used as the derived `rate_limit_config::rate_per_second`.
inline constexpr double default_smart_endpoint_rate_per_second = 1000.0;

// The concurrent-request limit assumed for one endpoint. Used as the derived
// `bulkhead_config::capacity`.
inline constexpr u32 default_smart_endpoint_concurrency = 1024;

// The per-call facts a smart policy is derived from.
//
// `deadline` is a *relative* duration: the time the caller allows for the whole
// logical call. A non-positive value means "no explicit deadline", in which case
// the derived timeout falls back to `default_smart_timeout`.
//
// `latency_slo` is the caller's target latency. It is carried so the adaptive
// path (the runtime p99 update in the circuit breaker) has the caller's intent;
// the derivation in this task does not consume it.
struct call_traits {
    bool idempotent = false;
    std::chrono::nanoseconds deadline{std::chrono::nanoseconds::zero()};
    std::chrono::nanoseconds latency_slo{std::chrono::nanoseconds::zero()};
};

// The derived policy. Every field is a ready-to-use configuration for the
// corresponding resilience primitive.
struct smart_policy {
    std::chrono::nanoseconds timeout{default_smart_timeout};
    retry_policy retry{};
    breaker_config breaker{};
    rate_limit_config rate{};
    bulkhead_config bulkhead{};
};

// Derives a policy from `traits`. Never blocks and never allocates.
[[nodiscard]] CRPC_API smart_policy make_smart_policy(
    const call_traits& traits) noexcept;

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_SMART_POLICY_HPP
