// CRPC -- diagnostics tests.
//
// Task 1 of the diagnostics workflow: `rpc_error` carries the origin of a
// terminal failure and the number of attempts the logical call made. The
// `failure_origin` enum lives in `error.hpp` (the core error header) so that
// `rpc_error` can name it without pulling in the client facade.
#include <crpc/diagnostics.hpp>
#include <crpc/error.hpp>

#include <string>

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
    const expectation cases[] = {
        {crpc::failure_origin::local_lifecycle, true,  false, false, false, false, true,  false},
        {crpc::failure_origin::local_gate,       true,  false, false, true,  false, false, false},
        {crpc::failure_origin::local_capacity,   true,  false, false, false, true,  false, false},
        {crpc::failure_origin::endpoint_open,    false, true,  false, false, false, false, false},
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

CRPC_TEST(diagnostics_predicates_are_origin_based_not_code_based) {
    // A remote origin carrying a local-looking code is still a server failure:
    // the predicates read `origin`, not `code`.
    crpc::rpc_error error(crpc::rpc_code::unavailable, "server rejected");
    error.origin = crpc::failure_origin::remote;
    CRPC_CHECK(crpc::is_server(error));
    CRPC_CHECK_FALSE(crpc::is_local(error));
}

CRPC_TEST_MAIN()
