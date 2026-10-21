// CRPC -- admission vocabulary and HTTP Retry-After contract.
//
// Slice 1 of 4c changes no behaviour. These checks pin the two things that are
// easy to drift silently:
//   1. the single stage -> rpc_code table, including both rate outcomes;
//   2. the Retry-After projection of rpc_error::retry_after.
// The field-by-field before/after comparison (code, retryable, retry_after,
// message, counter delta) is covered by the server/client regression suite.

#include <chrono>
#include <cstddef>
#include <string>

#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/mvc/router.hpp>
#include <crpc/resilience/admission.hpp>

// test_framework.hpp (Debug) issues a file-scope `#pragma section(".CRT$XCU",
// read)` that makes subsequent `static inline` data-member dynamic initializers
// inert under MSVC. Every CRPC header this file needs is included above, so the
// pragma cannot reach them. This mirrors tests/test_mvc.cpp.
#undef _DEBUG
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

namespace admission = crpc::resilience;

// The table itself is a compile-time contract: a change here must be a
// deliberate wire change, not a refactoring side effect.
static_assert(admission::reject_code(admission::stage_kind::breaker) ==
              crpc::rpc_code::unavailable);
static_assert(admission::reject_code(admission::stage_kind::service_bulkhead) ==
              crpc::rpc_code::overload);
static_assert(admission::reject_code(admission::stage_kind::service_capacity) ==
              crpc::rpc_code::overload);
static_assert(admission::reject_code(admission::stage_kind::service_rate, true) ==
              crpc::rpc_code::resource_exhausted);
static_assert(admission::reject_code(admission::stage_kind::service_rate, false) ==
              crpc::rpc_code::deadline_exceeded);

void check_retry_after(crpc::rpc_code code, std::chrono::milliseconds hint,
                       const char* expected, int expected_status) {
    crpc::rpc_error error(code, "scripted");
    error.retry_after = hint;
    crpc::http::response response;
    crpc::mvc::write_error(response, error, "/admission");
    CRPC_CHECK_EQ(expected_status, response.status_code);
    const std::string* header = response.headers.find("Retry-After");
    if (expected == nullptr) {
        CRPC_CHECK(header == nullptr);
        return;
    }
    CRPC_CHECK(header != nullptr);
    if (header != nullptr) {
        CRPC_CHECK_EQ(std::string(expected), *header);
    }
}

} // namespace

CRPC_TEST(admission_reject_code_contract) {
    // Both rate outcomes share one branch and differ only in the deadline.
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_rate) ==
               crpc::rpc_code::resource_exhausted);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_rate, true) ==
               crpc::rpc_code::resource_exhausted);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_rate, false) ==
               crpc::rpc_code::deadline_exceeded);

    CRPC_CHECK(admission::reject_code(admission::stage_kind::connection_rate) ==
               crpc::rpc_code::resource_exhausted);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::breaker) ==
               crpc::rpc_code::unavailable);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::dispatch) ==
               crpc::rpc_code::unavailable);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_bulkhead) ==
               crpc::rpc_code::overload);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_capacity) ==
               crpc::rpc_code::overload);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::local_gate) ==
               crpc::rpc_code::resource_exhausted);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::deadline) ==
               crpc::rpc_code::deadline_exceeded);

    // Only the rate stage reads the deadline: every other stage must answer
    // the same code for both values.
    CRPC_CHECK(admission::reject_code(admission::stage_kind::breaker, false) ==
               crpc::rpc_code::unavailable);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_bulkhead,
                                      false) == crpc::rpc_code::overload);
    CRPC_CHECK(admission::reject_code(admission::stage_kind::service_capacity,
                                      false) == crpc::rpc_code::overload);
}

CRPC_TEST(admission_fallback_eligible_one_way) {
    // `one_way` is a call shape, not a failure: a one-way call has no response
    // frame, so it can never be degraded.
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::unavailable, admission::stage_kind::breaker, true));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::overload, admission::stage_kind::service_bulkhead, true));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::resource_exhausted, admission::stage_kind::service_rate,
        true));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::ok, admission::stage_kind::breaker, true));

    // A deadline miss is never degraded, whatever the stage.
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::deadline_exceeded, admission::stage_kind::service_rate,
        false));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::deadline_exceeded, admission::stage_kind::deadline,
        false));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::deadline_exceeded, admission::stage_kind::dispatch,
        false));
}

CRPC_TEST(admission_fallback_eligible_stage_pairs) {
    CRPC_CHECK(admission::fallback_eligible(
        crpc::rpc_code::unavailable, admission::stage_kind::breaker, false));
    CRPC_CHECK(admission::fallback_eligible(
        crpc::rpc_code::unavailable, admission::stage_kind::dispatch, false));
    CRPC_CHECK(admission::fallback_eligible(
        crpc::rpc_code::overload, admission::stage_kind::service_bulkhead, false));
    CRPC_CHECK(admission::fallback_eligible(
        crpc::rpc_code::overload, admission::stage_kind::service_capacity, false));
    CRPC_CHECK(admission::fallback_eligible(
        crpc::rpc_code::resource_exhausted, admission::stage_kind::service_rate,
        false));
    CRPC_CHECK(admission::fallback_eligible(
        crpc::rpc_code::resource_exhausted,
        admission::stage_kind::service_capacity, false));

    // Same code, wrong stage: the pair is what matters.
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::unavailable, admission::stage_kind::service_rate, false));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::overload, admission::stage_kind::service_rate, false));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::resource_exhausted, admission::stage_kind::breaker, false));
    CRPC_CHECK_FALSE(admission::fallback_eligible(
        crpc::rpc_code::ok, admission::stage_kind::breaker, false));
}

CRPC_TEST(mvc_write_error_retry_after_rounds_up_to_one_second) {
    check_retry_after(crpc::rpc_code::overload, 1500ms, "2", 429);
    check_retry_after(crpc::rpc_code::overload, 1ms, "1", 429);
    check_retry_after(crpc::rpc_code::overload, 1000ms, "1", 429);
    check_retry_after(crpc::rpc_code::overload, 1001ms, "2", 429);
}

CRPC_TEST(mvc_write_error_omits_retry_after_when_zero) {
    check_retry_after(crpc::rpc_code::overload, 0ms, nullptr, 429);
    check_retry_after(crpc::rpc_code::resource_exhausted, 0ms, nullptr, 429);
    check_retry_after(crpc::rpc_code::deadline_exceeded, 0ms, nullptr, 504);
}

CRPC_TEST(mvc_write_error_keeps_status_mapping) {
    check_retry_after(crpc::rpc_code::resource_exhausted, 1500ms, "2", 429);
    check_retry_after(crpc::rpc_code::unavailable, 2000ms, "2", 503);
    check_retry_after(crpc::rpc_code::deadline_exceeded, 5000ms, "5", 504);
    check_retry_after(crpc::rpc_code::application, 1500ms, "2", 500);
}

CRPC_TEST_MAIN()
