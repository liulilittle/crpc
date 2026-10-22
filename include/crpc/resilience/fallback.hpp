// CRPC -- fallback vocabulary and the single value entry point.
//
// This header is deliberately header-only: no exported symbols, no layout
// change, no behaviour. Slice 2 of 4c adds the primitives only; nothing in
// `src/` calls them yet. The slice that consumes them (S4) runs the fallback
// after the last attempt has been finalized and its breaker outcome recorded.
//
// Flow
// ----
// `fallback_execute` is the only function that calls a `fallback_fn`:
//   G0  eligibility: the S1 table, the "sent implies idempotent" gate, and a
//       stage that the caller must state explicitly;
//   G1  entry gate: skip when the remaining time is below the policy floor;
//   G2  execution: the function writes one value into `out`;
//   G3  overrun check: a function that ran past the deadline cannot produce a
//       value.
//
// G1 and G3 are deliberately NOT symmetric:
//   G1 compares `deadline - now > min_remaining`, because a fallback that
//   cannot finish inside the floor should not start at all;
//   G3 compares only `now >= deadline`, because "degraded after the deadline"
//   is the only thing that may be reported as a timeout. Reusing the floor at
//   G3 would turn a value returned with 40 ms left into `deadline_exceeded`,
//   which is not retryable, and would therefore forbid a retry of a call that
//   actually succeeded. See docs/architecture.md (fallback, unwired).
//
// G3 rewrites `error` to `deadline_exceeded` on an overrun, preserving only the
// client-local `unknown_side_effect` marker. A caller that returns that error
// therefore answers with the timeout semantics, never with the original
// failure. A function that merely fails leaves `error` untouched, so the
// breaker keeps attributing the original failure to the endpoint.
//
// Counting
// --------
// The counters live behind a `fallback_counter_sink`. `fallback_execute`
// increments `attempted` right before it calls the function and exactly one
// terminal counter before every return, so a consumer cannot forget a
// denominator. `ineligible` also covers an entry-time cancellation: the two
// cases share one counter. A consumer that needs an eligibility rate must
// subtract the entry-cancel case, and a consumer that needs the distinction
// must test `canceled` before calling.
//
// `sink->self` is NOT kept alive and its lifetime is not constrained by this
// API. A consumer must point it at an object that outlives the synchronous
// call (for the client, the `call_state` that owns the shared attempt state),
// never at a raw `client*` or `impl*`: a concurrent close() may free those
// while the function runs.
//
// Clocks are injected so the deadline boundary is deterministic in tests. The
// default is the same `std::chrono::steady_clock` the client already uses.
#ifndef CRPC_RESILIENCE_FALLBACK_HPP
#define CRPC_RESILIENCE_FALLBACK_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <crpc/buffer.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/resilience/admission.hpp>

namespace crpc::resilience {

// The two response metadata keys that describe a degraded result. Both are
// reserved: the admission stages set neither. `degraded()` / `degradable()`
// only test for the key's presence; the value is documentation for a human
// reading the frame.
inline constexpr char degraded_metadata_key[] = "crpc-degraded";
inline constexpr char degradable_metadata_key[] = "crpc-degradable";
inline constexpr char degraded_metadata_value[] = "true";
// Client-local marker set by the low-level client when a sent, non-idempotent
// request timed out (src/rpc/client.cpp, `error.metadata.set(...)`). A fallback
// overrun must keep it: the peer may already have executed the request, and a
// timeout does not undo that fact.
inline constexpr char unknown_side_effect_metadata_key[] = "unknown_side_effect";

[[nodiscard]] inline bool degraded(const rpc_error& error) noexcept {
    return error.metadata.find(degraded_metadata_key) != nullptr;
}

[[nodiscard]] inline bool degradable(const rpc_error& error) noexcept {
    return error.metadata.find(degradable_metadata_key) != nullptr;
}

using fallback_clock = std::chrono::steady_clock::time_point (*)() noexcept;

[[nodiscard]] inline std::chrono::steady_clock::time_point
steady_clock_now() noexcept {
    return std::chrono::steady_clock::now();
}

// Everything a fallback function may look at. The remaining time is NOT one of
// them: a stored "remaining" value would already be stale by the time the
// function ran. The caller compares the deadline through fallback_has_room()
// and the function decides on its own, bounded work.
struct fallback_context {
    rpc_code code = rpc_code::ok;
    // REQUIRED. `nullopt` means "the caller did not say where it failed" and is
    // always ineligible. Defaulting to `dispatch` would let a forgotten stage
    // silently match the legal (unavailable, dispatch) pair and fail open.
    std::optional<stage_kind> stage{};
    std::chrono::milliseconds retry_after{0};
    bool one_way = false;      // ctx.mode != delivery_mode::reliable
    bool sent = false;         // origin in {endpoint_sent, remote}
    bool idempotent = false;   // core.idempotent
    bool has_deadline = false;
    std::chrono::steady_clock::time_point deadline{};
};

// A fallback writes one encoded value into `out`. `noexcept` is part of the
// type: the primitive runs after the attempt was finalized and must not let an
// exception escape into the caller's terminal path.
using fallback_fn = status (*)(void* self, const fallback_context&,
                               buffer& out) noexcept;

struct fallback_policy {
    fallback_fn function = nullptr;
    void* self = nullptr;
    // G1 floor: a fallback that would leave less than this is not attempted.
    // Zero means "skip only when the deadline has already passed". G3 never
    // reads it -- see fallback_overran().
    std::chrono::nanoseconds min_remaining{0};
};

enum class fallback_outcome : u8 {
    not_configured = 0,
    ineligible,
    skipped_deadline,
    failed,
    canceled,
    overrun,
    applied
};
inline constexpr std::size_t fallback_outcome_count = 7;

enum class fallback_counter : u8 {
    ineligible = 0,
    skipped_deadline,
    attempted,
    failed,
    canceled,
    overrun,
    applied
};
inline constexpr std::size_t fallback_counter_count = 7;
static_assert(static_cast<u8>(fallback_counter::applied) + 1 ==
                  fallback_counter_count,
              "fallback_counter must stay dense and ordered");

// The only counting interface. `nullptr` (the default) means "do not count" and
// costs one comparison. Both increments live inside `fallback_execute`, so a
// consumer cannot forget `attempted`. See the header comment for the
// lifetime rule on `self`.
struct fallback_counter_sink {
    void* self = nullptr;
    void (*add)(void* self, fallback_counter) noexcept = nullptr;
};

inline void fallback_record(fallback_counter_sink* sink,
                            fallback_counter counter) noexcept {
    if (sink != nullptr && sink->add != nullptr) {
        sink->add(sink->self, counter);
    }
}

// G0: the S1 table, the mandatory idempotency gate on a sent path, and an
// explicit stage. A sent, non-idempotent request may already have produced a
// side effect, so it can never be replaced by a fallback value. A missing stage
// means the caller does not know which gate refused, so nothing is degraded.
[[nodiscard]] constexpr bool fallback_admissible(
    rpc_code code, std::optional<stage_kind> stage, bool one_way, bool sent,
    bool idempotent) noexcept {
    if (!stage.has_value()) {
        return false;
    }
    return fallback_eligible(code, *stage, one_way) && !(sent && !idempotent);
}

// G1: true when there is no deadline or when the time left is strictly above
// the floor. Equality is already too late, matching a caller that would have to
// answer deadline_exceeded.
//
// A negative floor is clamped to zero so a passed deadline is never admitted:
// `deadline - now > -1s` holds after the deadline, which would call fn and
// count an attempt that the deadline forbids (D7).
[[nodiscard]] inline bool fallback_has_room(
    const fallback_context& ctx, std::chrono::nanoseconds min_remaining,
    fallback_clock clock) noexcept {
    if (!ctx.has_deadline) {
        return true;
    }
    const auto floor = min_remaining > std::chrono::nanoseconds::zero()
                           ? min_remaining
                           : std::chrono::nanoseconds::zero();
    return ctx.deadline - clock() > floor;
}

// G3: true when the deadline has passed. Deliberately ignores the G1 floor: a
// value returned with time left is a success, even if less time is left than
// the policy would have required to start. Reporting it as deadline_exceeded
// would turn a degraded success into a non-retryable timeout.
[[nodiscard]] inline bool fallback_overran(
    const fallback_context& ctx, fallback_clock clock) noexcept {
    return ctx.has_deadline && clock() >= ctx.deadline;
}

// Rewrites the error for a G3 overrun. The original failure is gone -- the
// caller must answer deadline_exceeded, not the code that triggered the
// fallback -- but the client-local `unknown_side_effect` marker survives: it
// describes what may have happened at the peer, and a timeout does not
// retroactively make that false. Every other metadata entry is dropped.
inline void fallback_apply_overrun(rpc_error& error) noexcept {
    metadata_list old = std::move(error.metadata);
    error = rpc_error(rpc_code::deadline_exceeded,
                      "fallback overran the deadline");
    if (const std::string* value = old.find(unknown_side_effect_metadata_key)) {
        error.metadata.set(unknown_side_effect_metadata_key, *value);
    }
}

// The only function that calls `fallback_fn`. It runs G0 -> G1 -> G2 -> G3 and
// returns exactly one outcome, recording it through `sink` (when non-null).
//
// Out/error contract:
//   not_configured   out and error untouched (zero-cost fast path, no counter);
//   ineligible       out cleared, error untouched;
//   skipped_deadline out cleared, error untouched;
//   failed           out cleared, error untouched (breaker attribution stays);
//   canceled         out cleared, error untouched (caller reports canceled);
//   overrun          out cleared, error replaced by deadline_exceeded;
//   applied          value in out, error untouched (S4 marks the call degraded).
//
// `applied` means only "the function returned ok and the value is not late".
// It does not promise that `out` decodes: a function that returns ok without
// writing anything yields an empty body, and the consumer's own decode step
// then fails with `decode_error`. A consumer must therefore mark the call
// degraded only after its decode succeeded, not on `applied` alone.
//
// Cancellation is checked once at entry and once after the function returns.
// An entry-time cancellation is reported as `ineligible`, not `canceled`.
[[nodiscard]] inline fallback_outcome fallback_execute(
    const fallback_policy& policy, const fallback_context& ctx,
    const std::atomic<bool>& canceled, buffer& out, rpc_error& error,
    fallback_clock clock = &steady_clock_now,
    fallback_counter_sink* sink = nullptr) noexcept {
    if (policy.function == nullptr) {
        return fallback_outcome::not_configured;
    }

    // Every other path returns with `out` empty, so a consumer can never read a
    // stale value from an earlier attempt. `clear()` keeps the capacity.
    out.clear();

    if (!fallback_admissible(ctx.code, ctx.stage, ctx.one_way, ctx.sent,
                             ctx.idempotent) ||
        canceled.load(std::memory_order_acquire)) {
        fallback_record(sink, fallback_counter::ineligible);
        return fallback_outcome::ineligible;
    }
    if (!fallback_has_room(ctx, policy.min_remaining, clock)) {
        fallback_record(sink, fallback_counter::skipped_deadline);
        return fallback_outcome::skipped_deadline;
    }

    fallback_record(sink, fallback_counter::attempted);
    const status result = policy.function(policy.self, ctx, out);
    if (!result.ok()) {
        out.clear();
        fallback_record(sink, fallback_counter::failed);
        return fallback_outcome::failed;
    }
    // Cancellation wins over an overrun: the caller is going to report
    // `canceled` either way and the value must not survive.
    if (canceled.load(std::memory_order_acquire)) {
        out.clear();
        fallback_record(sink, fallback_counter::canceled);
        return fallback_outcome::canceled;
    }
    if (fallback_overran(ctx, clock)) {
        out.clear();
        fallback_apply_overrun(error);
        fallback_record(sink, fallback_counter::overrun);
        return fallback_outcome::overrun;
    }
    fallback_record(sink, fallback_counter::applied);
    return fallback_outcome::applied;
}

// One counter per outcome except not_configured, which never reaches a counter.
// `attempted` is not an outcome: it is incremented before the function runs.
struct fallback_statistics {
    std::size_t ineligible = 0;
    std::size_t skipped_deadline = 0;
    std::size_t attempted = 0;
    std::size_t failed = 0;
    std::size_t canceled = 0;
    std::size_t overrun = 0;
    std::size_t applied = 0;
};

[[nodiscard]] constexpr bool counted(fallback_outcome outcome) noexcept {
    return outcome != fallback_outcome::not_configured;
}

// True for the four terminal outcomes of an actual call. `attempted` is
// incremented separately, right before the function runs, so the per-event
// relation attempted == failed + canceled + overrun + applied holds without a
// snapshot claiming cross-counter consistency.
[[nodiscard]] constexpr bool counts_as_attempt(fallback_outcome outcome) noexcept {
    switch (outcome) {
    case fallback_outcome::failed:
    case fallback_outcome::canceled:
    case fallback_outcome::overrun:
    case fallback_outcome::applied:
        return true;
    default:
        return false;
    }
}

// Precondition: counted(outcome). not_configured has no counter; a caller must
// gate on counted() before using the result. Every other outcome maps 1:1.
[[nodiscard]] constexpr fallback_counter counter_for(
    fallback_outcome outcome) noexcept {
    switch (outcome) {
    case fallback_outcome::ineligible:
        return fallback_counter::ineligible;
    case fallback_outcome::skipped_deadline:
        return fallback_counter::skipped_deadline;
    case fallback_outcome::failed:
        return fallback_counter::failed;
    case fallback_outcome::canceled:
        return fallback_counter::canceled;
    case fallback_outcome::overrun:
        return fallback_counter::overrun;
    case fallback_outcome::applied:
        return fallback_counter::applied;
    case fallback_outcome::not_configured:
        break;
    }
    // Unreachable under the precondition; returning a real counter keeps the
    // function constexpr-friendly without inventing a sentinel enum value.
    return fallback_counter::ineligible;
}

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_FALLBACK_HPP
