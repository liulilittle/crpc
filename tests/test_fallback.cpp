// CRPC -- fallback primitives (4c slice 2, v2).
//
// Slice 2 adds the fallback vocabulary but wires nothing into a call path.
// These checks pin the primitive itself:
//   1. the eligibility gate is exactly fallback_eligible() plus the
//      idempotency gate plus an explicitly stated stage;
//   2. an injected clock makes the entry floor and the overrun deterministic,
//      and the two boundaries are deliberately different;
//   3. a failed function leaves the original error intact, an overrun rewrites
//      it to deadline_exceeded, and a cancellation never leaks a value;
//   4. every non-fast path clears `out` at entry;
//   5. the counter sink is incremented exactly once per outcome;
//   6. both metadata keys survive an error-envelope round trip;
//   7. the function pointer type enforces noexcept at compile time.
// The zero-wiring check against a live client lives in test_server_client.cpp.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include <crpc/buffer.hpp>
#include <crpc/error.hpp>
#include <crpc/resilience/admission.hpp>
#include <crpc/resilience/fallback.hpp>
#include <crpc/rpc/dispatcher.hpp>

// test_framework.hpp (Debug) issues a file-scope pragma that makes later
// `static inline` data-member initializers inert under MSVC. Every CRPC header
// this file needs is included above, so the pragma cannot reach them. This
// mirrors tests/test_admission.cpp.
#undef _DEBUG
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

namespace fb = crpc::resilience;

constexpr crpc::rpc_code k_codes[] = {
    crpc::rpc_code::ok,
    crpc::rpc_code::unavailable,
    crpc::rpc_code::overload,
    crpc::rpc_code::resource_exhausted,
    crpc::rpc_code::deadline_exceeded,
    crpc::rpc_code::connection_reset,
    crpc::rpc_code::internal,
    crpc::rpc_code::application,
};
static_assert(sizeof(k_codes) / sizeof(k_codes[0]) == 8,
              "update k_codes when the table changes");

constexpr fb::stage_kind k_stages[] = {
    fb::stage_kind::connection_rate,
    fb::stage_kind::service_rate,
    fb::stage_kind::service_bulkhead,
    fb::stage_kind::service_capacity,
    fb::stage_kind::endpoint_rate,
    fb::stage_kind::endpoint_bulkhead,
    fb::stage_kind::breaker,
    fb::stage_kind::dispatch,
    fb::stage_kind::deadline,
    fb::stage_kind::pool_capacity,
};
static_assert(sizeof(k_stages) / sizeof(k_stages[0]) == 10,
              "update k_stages when a stage_kind is added");

std::chrono::steady_clock::time_point g_fake_now{};

std::chrono::steady_clock::time_point fake_clock() noexcept { return g_fake_now; }

struct fn_state {
    int calls = 0;
    bool write_value = true;
    crpc::rpc_code result = crpc::rpc_code::ok;
    bool cancel_during = false;
    std::atomic<bool>* canceled = nullptr;
    bool move_clock = false;
    std::chrono::steady_clock::time_point new_now{};
};

crpc::status scripted_fallback(void* self, const fb::fallback_context&,
                               crpc::buffer& out) noexcept {
    fn_state& state = *static_cast<fn_state*>(self);
    ++state.calls;
    if (state.move_clock) {
        g_fake_now = state.new_now;
    }
    if (state.write_value) {
        (void)out.append("value", 5);
    }
    if (state.cancel_during && state.canceled != nullptr) {
        state.canceled->store(true, std::memory_order_release);
    }
    if (state.result != crpc::rpc_code::ok) {
        return crpc::status{state.result};
    }
    return crpc::status{};
}

crpc::status noexcept_fallback(void*, const fb::fallback_context&,
                               crpc::buffer&) noexcept {
    return crpc::status{};
}

[[maybe_unused]] crpc::status not_noexcept_fallback(void*,
                                                    const fb::fallback_context&,
                                                    crpc::buffer&) {
    return crpc::status{};
}

fb::fallback_context context_for(crpc::rpc_code code, fb::stage_kind stage) {
    fb::fallback_context ctx;
    ctx.code = code;
    ctx.stage = stage;
    return ctx;
}

fb::fallback_policy policy_for(fn_state& state) {
    fb::fallback_policy policy;
    policy.function = &scripted_fallback;
    policy.self = &state;
    return policy;
}

// Counts every increment so a test can assert "exactly once" per outcome.
struct counter_box {
    std::size_t ineligible = 0;
    std::size_t skipped_deadline = 0;
    std::size_t attempted = 0;
    std::size_t failed = 0;
    std::size_t canceled = 0;
    std::size_t overrun = 0;
    std::size_t applied = 0;

    fb::fallback_counter_sink sink() {
        fb::fallback_counter_sink out;
        out.self = this;
        out.add = &counter_box::add;
        return out;
    }

    static void add(void* self, fb::fallback_counter counter) noexcept {
        counter_box& box = *static_cast<counter_box*>(self);
        switch (counter) {
        case fb::fallback_counter::ineligible:
            ++box.ineligible;
            break;
        case fb::fallback_counter::skipped_deadline:
            ++box.skipped_deadline;
            break;
        case fb::fallback_counter::attempted:
            ++box.attempted;
            break;
        case fb::fallback_counter::failed:
            ++box.failed;
            break;
        case fb::fallback_counter::canceled:
            ++box.canceled;
            break;
        case fb::fallback_counter::overrun:
            ++box.overrun;
            break;
        case fb::fallback_counter::applied:
            ++box.applied;
            break;
        }
    }
};

void check_error_unchanged(const crpc::rpc_error& expected,
                           const crpc::rpc_error& actual) {
    CRPC_CHECK(expected.code == actual.code);
    CRPC_CHECK_EQ(expected.message, actual.message);
    CRPC_CHECK_EQ(expected.retryable, actual.retryable);
    CRPC_CHECK_EQ(expected.retry_after.count(), actual.retry_after.count());
    CRPC_CHECK_EQ(expected.metadata.size(), actual.metadata.size());
    for (const auto& entry : expected.metadata) {
        const std::string* value = actual.metadata.find(entry.first);
        CRPC_CHECK(value != nullptr);
        if (value != nullptr) {
            CRPC_CHECK_EQ(entry.second, *value);
        }
    }
}

crpc::rpc_error decorated_error(crpc::rpc_code code) {
    crpc::rpc_error error(code, "original");
    error.retryable = true;
    error.retry_after = 25ms;
    error.metadata.set("application.key", "application.value");
    return error;
}

crpc::buffer buffer_with_residue() {
    crpc::buffer out;
    (void)out.append("keep", 4);
    return out;
}

} // namespace

// --- Compile-time contract ---------------------------------------------------

static_assert(std::is_same<fb::fallback_clock,
                           std::chrono::steady_clock::time_point (*)() noexcept>::value,
              "fallback_clock must be a noexcept clock function pointer");
static_assert(std::is_convertible<decltype(&noexcept_fallback),
                                  fb::fallback_fn>::value,
              "a noexcept function must convert to fallback_fn");
static_assert(!std::is_convertible<decltype(&not_noexcept_fallback),
                                   fb::fallback_fn>::value,
              "a potentially throwing function must not convert to fallback_fn");
static_assert(std::is_trivially_copyable<fb::fallback_context>::value,
              "fallback_context must be trivially copyable");
static_assert(std::is_standard_layout<fb::fallback_context>::value,
              "fallback_context must be standard-layout");
static_assert(std::is_trivially_copyable<fb::fallback_policy>::value,
              "fallback_policy must be trivially copyable");
static_assert(std::is_standard_layout<fb::fallback_statistics>::value,
              "fallback_statistics must be standard-layout");
static_assert(sizeof(fb::fallback_statistics) == 7 * sizeof(std::size_t),
              "fallback_statistics must be exactly seven counters");
static_assert(fb::fallback_outcome_count == 7,
              "one count per fallback_outcome value");
static_assert(fb::fallback_counter_count == 7,
              "one counter per countable outcome");
static_assert(noexcept(fb::steady_clock_now()), "steady_clock_now must not throw");
static_assert(noexcept(fb::degraded(std::declval<const crpc::rpc_error&>())),
              "degraded must not throw");
static_assert(noexcept(fb::degradable(std::declval<const crpc::rpc_error&>())),
              "degradable must not throw");
static_assert(noexcept(fb::fallback_admissible(
                  crpc::rpc_code::ok, fb::stage_kind::dispatch, false, false,
                  false)),
              "fallback_admissible must not throw");

// --- 1. Truth table ----------------------------------------------------------

CRPC_TEST(fallback_gate_matches_eligible_and_idempotency) {
    for (const crpc::rpc_code code : k_codes) {
        for (const fb::stage_kind stage : k_stages) {
            for (int flags = 0; flags < 8; ++flags) {
                const bool one_way = (flags & 1) != 0;
                const bool sent = (flags & 2) != 0;
                const bool idempotent = (flags & 4) != 0;
                const bool expected =
                    fb::fallback_eligible(code, stage, one_way) &&
                    !(sent && !idempotent);
                CRPC_CHECK_EQ(expected,
                              fb::fallback_admissible(code, stage, one_way, sent,
                                                      idempotent));

                fn_state state;
                fb::fallback_policy policy = policy_for(state);
                fb::fallback_context ctx = context_for(code, stage);
                ctx.one_way = one_way;
                ctx.sent = sent;
                ctx.idempotent = idempotent;
                std::atomic<bool> canceled{false};
                crpc::rpc_error error(code, "original");
                crpc::buffer out = buffer_with_residue();
                const fb::fallback_outcome outcome = fb::fallback_execute(
                    policy, ctx, canceled, out, error, &fake_clock);
                if (expected) {
                    CRPC_CHECK(outcome == fb::fallback_outcome::applied);
                    CRPC_CHECK_EQ(1, state.calls);
                    CRPC_CHECK_EQ(static_cast<std::size_t>(5), out.size());
                } else {
                    CRPC_CHECK(outcome == fb::fallback_outcome::ineligible);
                    CRPC_CHECK_EQ(0, state.calls);
                    CRPC_CHECK(out.empty());
                }
                CRPC_CHECK(error.code == code);
            }
        }
    }
}

CRPC_TEST(fallback_gate_rejects_deadline_and_load_shedding) {
    // A deadline miss is never degraded, whatever the stage.
    for (const fb::stage_kind stage : k_stages) {
        CRPC_CHECK_FALSE(fb::fallback_admissible(
            crpc::rpc_code::deadline_exceeded, stage, false, false, true));
    }
    // The per-connection bucket sheds load before a job exists.
    CRPC_CHECK_FALSE(fb::fallback_admissible(
        crpc::rpc_code::resource_exhausted, fb::stage_kind::connection_rate,
        false, false, true));
    // A sent, non-idempotent call may have produced a side effect.
    CRPC_CHECK_FALSE(fb::fallback_admissible(
        crpc::rpc_code::unavailable, fb::stage_kind::breaker, false, true,
        false));
    CRPC_CHECK(fb::fallback_admissible(crpc::rpc_code::unavailable,
                                       fb::stage_kind::breaker, false, true,
                                       true));
}

// --- 2. A missing stage is never eligible ------------------------------------

CRPC_TEST(fallback_missing_stage_is_ineligible) {
    // (unavailable, dispatch) is a legal pair, so defaulting the stage to
    // dispatch would silently degrade a remote failure. `nullopt` must refuse.
    CRPC_CHECK_FALSE(fb::fallback_admissible(
        crpc::rpc_code::unavailable, std::nullopt, false, false, true));

    fb::fallback_context ctx;
    ctx.code = crpc::rpc_code::unavailable;
    CRPC_CHECK_FALSE(ctx.stage.has_value());
    fn_state state;
    fb::fallback_policy policy = policy_for(state);
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;
    crpc::buffer out = buffer_with_residue();
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::ineligible);
    CRPC_CHECK_EQ(0, state.calls);
    CRPC_CHECK(out.empty());
    check_error_unchanged(original, error);
}

// --- 3. Injected clock: G1 floor and G3 deadline are different ---------------

CRPC_TEST(fallback_entry_floor_uses_the_injected_clock) {
    const auto t0 = std::chrono::steady_clock::time_point{} + 2000s;
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    ctx.has_deadline = true;
    ctx.deadline = t0;
    fn_state state;
    fb::fallback_policy policy = policy_for(state);
    policy.min_remaining = 5ms;
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;
    crpc::buffer out = buffer_with_residue();

    // Equality is already too late for the entry floor.
    g_fake_now = t0 - 5ms;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::skipped_deadline);
    CRPC_CHECK_EQ(0, state.calls);
    CRPC_CHECK(out.empty());
    check_error_unchanged(original, error);

    g_fake_now = t0 - 6ms;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::applied);
    CRPC_CHECK_EQ(1, state.calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), out.size());
}

CRPC_TEST(fallback_has_room_is_the_entry_floor_only) {
    // The strict `deadline - now > min_remaining` predicate gates the entry
    // only. It is not the overrun rule; see the next case.
    const auto t0 = std::chrono::steady_clock::time_point{} + 3000s;
    const std::chrono::nanoseconds floor = 5ms;
    fb::fallback_context ctx;
    ctx.has_deadline = false;
    CRPC_CHECK(fb::fallback_has_room(ctx, floor, &fake_clock));

    ctx.has_deadline = true;
    ctx.deadline = t0;
    g_fake_now = t0 - floor - 1ns;
    CRPC_CHECK(fb::fallback_has_room(ctx, floor, &fake_clock));
    g_fake_now = t0 - floor;
    CRPC_CHECK_FALSE(fb::fallback_has_room(ctx, floor, &fake_clock));
    g_fake_now = t0 - 1ns;
    CRPC_CHECK_FALSE(fb::fallback_has_room(ctx, floor, &fake_clock));

    // The overrun predicate only looks at the deadline, with no floor.
    g_fake_now = t0 - 1ns;
    CRPC_CHECK_FALSE(fb::fallback_overran(ctx, &fake_clock));
    g_fake_now = t0;
    CRPC_CHECK(fb::fallback_overran(ctx, &fake_clock));
    g_fake_now = t0 + 1s;
    CRPC_CHECK(fb::fallback_overran(ctx, &fake_clock));
}

CRPC_TEST(fallback_negative_floor_never_admits_a_passed_deadline) {
    // A negative floor must not turn "already past the deadline" into room:
    // `deadline - now > -1s` is true when 500 ms have already elapsed, which
    // would call fn after the deadline and count an attempt that D7 forbids.
    const auto t0 = std::chrono::steady_clock::time_point{} + 5000s;
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    ctx.has_deadline = true;
    ctx.deadline = t0;
    fn_state state;
    fb::fallback_policy policy = policy_for(state);
    policy.min_remaining = -1s;
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;
    crpc::buffer out = buffer_with_residue();

    g_fake_now = t0 + 500ms;
    CRPC_CHECK_FALSE(fb::fallback_has_room(ctx, policy.min_remaining, &fake_clock));
    counter_box box;
    fb::fallback_counter_sink sink = box.sink();
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::skipped_deadline);
    CRPC_CHECK_EQ(0, state.calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), box.attempted);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.skipped_deadline);
    CRPC_CHECK(out.empty());
    check_error_unchanged(original, error);

    // The clamp only removes the negative floor: with time left the same
    // policy still applies, so this is not a blanket refusal.
    g_fake_now = t0 - 1ms;
    CRPC_CHECK(fb::fallback_has_room(ctx, policy.min_remaining, &fake_clock));
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::applied);
    CRPC_CHECK_EQ(1, state.calls);
}

CRPC_TEST(fallback_g3_reads_only_the_deadline) {
    // Regression guard for the v2 P1: a value returned with 40 ms left is a
    // success even though the 50 ms entry floor would have refused to start.
    // Reporting it as deadline_exceeded would forbid a retry of a success.
    const auto t0 = std::chrono::steady_clock::time_point{} + 4000s;
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    ctx.has_deadline = true;
    ctx.deadline = t0 + 100ms;
    fn_state state;
    state.move_clock = true;
    state.new_now = t0 + 60ms;
    fb::fallback_policy policy = policy_for(state);
    policy.min_remaining = 50ms;
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;
    crpc::buffer out;

    g_fake_now = t0;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::applied);
    CRPC_CHECK_EQ(1, state.calls);
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), out.size());
    check_error_unchanged(original, error);
    CRPC_CHECK(error.code == crpc::rpc_code::unavailable);

    // Past the deadline it is an overrun: the value is dropped and the error
    // becomes a timeout.
    state.calls = 0;
    state.new_now = t0 + 101ms;
    g_fake_now = t0;
    crpc::buffer late;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, late, error,
                                    &fake_clock) ==
               fb::fallback_outcome::overrun);
    CRPC_CHECK_EQ(1, state.calls);
    CRPC_CHECK(late.empty());
    CRPC_CHECK(error.code == crpc::rpc_code::deadline_exceeded);
    CRPC_CHECK_EQ(crpc::is_retryable(crpc::rpc_code::deadline_exceeded),
                  error.retryable);
}

CRPC_TEST(fallback_overrun_keeps_the_unknown_side_effect_marker) {
    const auto t0 = std::chrono::steady_clock::time_point{} + 5000s;
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    ctx.has_deadline = true;
    ctx.deadline = t0;
    fn_state state;
    state.move_clock = true;
    state.new_now = t0 + 1ms;
    fb::fallback_policy policy = policy_for(state);
    std::atomic<bool> canceled{false};
    crpc::rpc_error error(crpc::rpc_code::unavailable, "transport failed");
    error.metadata.set("application.key", "application.value");
    error.metadata.set(fb::unknown_side_effect_metadata_key, "1");
    crpc::buffer out;

    g_fake_now = t0 - 1ms;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::overrun);
    CRPC_CHECK(error.code == crpc::rpc_code::deadline_exceeded);
    const std::string* marker =
        error.metadata.find(fb::unknown_side_effect_metadata_key);
    CRPC_CHECK(marker != nullptr);
    if (marker != nullptr) {
        CRPC_CHECK_EQ(std::string("1"), *marker);
    }
    CRPC_CHECK(error.metadata.find("application.key") == nullptr);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), error.metadata.size());
}

// --- 4. Rejection semantics --------------------------------------------------

CRPC_TEST(fallback_unconfigured_touches_nothing) {
    // The zero-cost fast path: no policy means no work at all, including no
    // clearing of the caller's buffer.
    fb::fallback_policy policy;
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;
    crpc::buffer out = buffer_with_residue();
    counter_box box;
    fb::fallback_counter_sink sink = box.sink();
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::not_configured);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), out.size());
    check_error_unchanged(original, error);
    CRPC_CHECK_FALSE(fb::counted(fb::fallback_outcome::not_configured));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), box.ineligible);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), box.attempted);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), box.applied);
}

CRPC_TEST(fallback_failure_keeps_the_original_error) {
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    fn_state state;
    state.result = crpc::rpc_code::decode_error;
    fb::fallback_policy policy = policy_for(state);
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;
    crpc::buffer out;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::failed);
    CRPC_CHECK_EQ(1, state.calls);
    CRPC_CHECK(out.empty());
    check_error_unchanged(original, error);
}

CRPC_TEST(fallback_cancellation_never_returns_a_value) {
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    fn_state state;
    fb::fallback_policy policy = policy_for(state);
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    const crpc::rpc_error original = error;

    // Already canceled at entry: ineligible, the function never runs. This
    // shares the `ineligible` counter with the table rejections.
    std::atomic<bool> canceled{true};
    crpc::buffer entry_out = buffer_with_residue();
    counter_box box;
    fb::fallback_counter_sink sink = box.sink();
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, entry_out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::ineligible);
    CRPC_CHECK_EQ(0, state.calls);
    CRPC_CHECK(entry_out.empty());
    check_error_unchanged(original, error);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.ineligible);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), box.attempted);

    // Canceled while the function runs: the value is discarded, the error is
    // left alone so the caller reports `canceled` from its own flag.
    std::atomic<bool> running{false};
    state.canceled = &running;
    state.cancel_during = true;
    crpc::buffer out;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, running, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::canceled);
    CRPC_CHECK_EQ(1, state.calls);
    CRPC_CHECK(out.empty());
    check_error_unchanged(original, error);
}

CRPC_TEST(fallback_success_leaves_the_error_alone) {
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::overload, fb::stage_kind::service_bulkhead);
    fn_state state;
    fb::fallback_policy policy = policy_for(state);
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::overload);
    const crpc::rpc_error original = error;
    crpc::buffer out;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                    &fake_clock) ==
               fb::fallback_outcome::applied);
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), out.size());
    check_error_unchanged(original, error);
}

// --- 5. Counter sink ---------------------------------------------------------

CRPC_TEST(fallback_sink_counts_each_outcome_exactly_once) {
    fb::fallback_context ctx =
        context_for(crpc::rpc_code::unavailable, fb::stage_kind::breaker);
    fn_state state;
    fb::fallback_policy policy = policy_for(state);
    std::atomic<bool> canceled{false};
    crpc::rpc_error error = decorated_error(crpc::rpc_code::unavailable);
    counter_box box;
    fb::fallback_counter_sink sink = box.sink();

    // Three successes: three attempts, three applied.
    for (int i = 0; i < 3; ++i) {
        crpc::buffer out;
        CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, out, error,
                                        &fake_clock, &sink) ==
                   fb::fallback_outcome::applied);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), box.attempted);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), box.applied);
    CRPC_CHECK_EQ(3, state.calls);

    // An ineligible outcome counts no attempt.
    fb::fallback_context bad = ctx;
    bad.one_way = true;
    crpc::buffer out;
    CRPC_CHECK(fb::fallback_execute(policy, bad, canceled, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::ineligible);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.ineligible);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), box.attempted);
    CRPC_CHECK_EQ(3, state.calls);

    // failed: one attempt, one failed.
    fn_state failing;
    failing.result = crpc::rpc_code::decode_error;
    fb::fallback_policy failing_policy = policy_for(failing);
    CRPC_CHECK(fb::fallback_execute(failing_policy, ctx, canceled, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::failed);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), box.attempted);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.failed);

    // canceled during the call: one attempt, one canceled.
    std::atomic<bool> running{false};
    fn_state canceling;
    canceling.canceled = &running;
    canceling.cancel_during = true;
    fb::fallback_policy canceling_policy = policy_for(canceling);
    CRPC_CHECK(fb::fallback_execute(canceling_policy, ctx, running, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::canceled);
    CRPC_CHECK_EQ(static_cast<std::size_t>(5), box.attempted);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.canceled);

    // overrun: one attempt, one overrun.
    const auto t0 = std::chrono::steady_clock::time_point{} + 6000s;
    fb::fallback_context timed = ctx;
    timed.has_deadline = true;
    timed.deadline = t0;
    fn_state late;
    late.move_clock = true;
    late.new_now = t0 + 1ms;
    fb::fallback_policy late_policy = policy_for(late);
    g_fake_now = t0 - 1ms;
    CRPC_CHECK(fb::fallback_execute(late_policy, timed, canceled, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::overrun);
    CRPC_CHECK_EQ(static_cast<std::size_t>(6), box.attempted);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.overrun);

    // skipped_deadline: no attempt.
    fn_state skipped;
    fb::fallback_policy skipped_policy = policy_for(skipped);
    skipped_policy.min_remaining = 5ms;
    g_fake_now = t0 - 5ms;
    CRPC_CHECK(fb::fallback_execute(skipped_policy, timed, canceled, out, error,
                                    &fake_clock, &sink) ==
               fb::fallback_outcome::skipped_deadline);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), box.skipped_deadline);
    CRPC_CHECK_EQ(static_cast<std::size_t>(6), box.attempted);
    CRPC_CHECK_EQ(0, skipped.calls);

    // The event relation holds for this single-threaded sequence.
    CRPC_CHECK_EQ(box.attempted, box.failed + box.canceled + box.overrun +
                                     box.applied);

    // A null sink is legal and does nothing.
    crpc::buffer plain;
    CRPC_CHECK(fb::fallback_execute(policy, ctx, canceled, plain, error,
                                    &fake_clock, nullptr) ==
               fb::fallback_outcome::applied);
}

// --- 6. Counter mapping ------------------------------------------------------

CRPC_TEST(fallback_outcome_counter_mapping_is_total) {
    struct row {
        fb::fallback_outcome outcome;
        bool counted;
        bool attempt;
        fb::fallback_counter counter;
    };
    const row rows[] = {
        {fb::fallback_outcome::not_configured, false, false,
         fb::fallback_counter::ineligible},
        {fb::fallback_outcome::ineligible, true, false,
         fb::fallback_counter::ineligible},
        {fb::fallback_outcome::skipped_deadline, true, false,
         fb::fallback_counter::skipped_deadline},
        {fb::fallback_outcome::failed, true, true, fb::fallback_counter::failed},
        {fb::fallback_outcome::canceled, true, true,
         fb::fallback_counter::canceled},
        {fb::fallback_outcome::overrun, true, true,
         fb::fallback_counter::overrun},
        {fb::fallback_outcome::applied, true, true, fb::fallback_counter::applied},
    };
    static_assert(sizeof(rows) / sizeof(rows[0]) == 7,
                  "one row per fallback_outcome value");
    CRPC_CHECK_EQ(static_cast<std::size_t>(7), fb::fallback_counter_count);
    for (const row& entry : rows) {
        CRPC_CHECK_EQ(entry.counted, fb::counted(entry.outcome));
        CRPC_CHECK_EQ(entry.attempt, fb::counts_as_attempt(entry.outcome));
        if (entry.counted) {
            CRPC_CHECK(entry.counter == fb::counter_for(entry.outcome));
        }
    }

    // The event accounting a consumer performs: `attempted` is a separate
    // increment, and every attempt ends in exactly one terminal counter.
    std::size_t counters[7] = {};
    std::size_t attempted = 0;
    const fb::fallback_outcome events[] = {
        fb::fallback_outcome::ineligible,
        fb::fallback_outcome::skipped_deadline,
        fb::fallback_outcome::failed,
        fb::fallback_outcome::canceled,
        fb::fallback_outcome::overrun,
        fb::fallback_outcome::applied,
    };
    for (const fb::fallback_outcome outcome : events) {
        if (!fb::counted(outcome)) {
            continue;
        }
        ++counters[static_cast<std::size_t>(fb::counter_for(outcome))];
        if (fb::counts_as_attempt(outcome)) {
            ++attempted;
        }
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                  counters[static_cast<std::size_t>(
                      fb::fallback_counter::ineligible)]);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                  counters[static_cast<std::size_t>(
                      fb::fallback_counter::skipped_deadline)]);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), attempted);
    const std::size_t terminal =
        counters[static_cast<std::size_t>(fb::fallback_counter::failed)] +
        counters[static_cast<std::size_t>(fb::fallback_counter::canceled)] +
        counters[static_cast<std::size_t>(fb::fallback_counter::overrun)] +
        counters[static_cast<std::size_t>(fb::fallback_counter::applied)];
    CRPC_CHECK_EQ(attempted, terminal);
}

// --- 7. Metadata carrier -----------------------------------------------------

CRPC_TEST(fallback_metadata_keys_survive_the_error_envelope) {
    crpc::rpc_error error(crpc::rpc_code::unavailable, "degraded");
    error.metadata.set(fb::degraded_metadata_key, fb::degraded_metadata_value);
    error.metadata.set(fb::degradable_metadata_key, fb::degraded_metadata_value);

    crpc::buffer wire;
    CRPC_CHECK(crpc::rpc::encode_response(crpc::rpc_code::unavailable, error, {},
                                          wire)
                   .ok());
    crpc::rpc_code code = crpc::rpc_code::ok;
    crpc::rpc_error decoded;
    crpc::span<const std::byte> body;
    CRPC_CHECK(crpc::rpc::decode_response(wire.readable(), code, decoded, body)
                   .ok());
    CRPC_CHECK(code == crpc::rpc_code::unavailable);
    CRPC_CHECK(fb::degraded(decoded));
    CRPC_CHECK(fb::degradable(decoded));

    crpc::rpc_error plain(crpc::rpc_code::unavailable, "plain");
    CRPC_CHECK_FALSE(fb::degraded(plain));
    CRPC_CHECK_FALSE(fb::degradable(plain));

    // The ok branch carries no error envelope: the metadata is dropped.
    crpc::buffer ok_wire;
    CRPC_CHECK(crpc::rpc::encode_response(crpc::rpc_code::ok, error, {}, ok_wire)
                   .ok());
    crpc::rpc_code ok_code = crpc::rpc_code::unavailable;
    crpc::rpc_error ok_error = decorated_error(crpc::rpc_code::unavailable);
    CRPC_CHECK(crpc::rpc::decode_response(ok_wire.readable(), ok_code, ok_error,
                                          body)
                   .ok());
    CRPC_CHECK(ok_code == crpc::rpc_code::ok);
    CRPC_CHECK_FALSE(fb::degraded(ok_error));
    CRPC_CHECK_FALSE(fb::degradable(ok_error));
    CRPC_CHECK(ok_error.metadata.empty());
}

CRPC_TEST_MAIN()
