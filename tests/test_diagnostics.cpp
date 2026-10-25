// CRPC -- diagnostics tests.
//
// Task 1 of the diagnostics workflow: `rpc_error` carries the origin of a
// terminal failure and the number of attempts the logical call made. The
// `failure_origin` enum lives in `error.hpp` (the core error header) so that
// `rpc_error` can name it without pulling in the client facade.
#include <crpc/error.hpp>

#include "test_framework.hpp"

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

CRPC_TEST_MAIN()
