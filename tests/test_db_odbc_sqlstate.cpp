// CRPC -- SQLSTATE classification.
//
// This runs without a driver: the mapping is pure logic, and it is the part of the ODBC
// bridge that decides whether a caller retries, gives up or fixes the statement.
#include "test_framework.hpp"

#include <string_view>

#include <crpc/db/odbc/connection.hpp>
#include <crpc/error.hpp>

namespace {

[[nodiscard]] crpc::rpc_code classify(std::string_view sqlstate) {
    return crpc::db::odbc::map_sqlstate(sqlstate);
}

} // namespace

CRPC_TEST(sqlstate_integrity_violations_are_preconditions) {
    // 23xxx: duplicate key, NOT NULL, foreign key. The caller must fix the data, so it is
    // not retryable.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(classify("23000")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(classify("23505")));
}

CRPC_TEST(sqlstate_connection_failures_are_unavailable) {
    // 08xxx: the link failed. Retrying on a fresh connection is correct.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(classify("08001")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(classify("08S01")));
}

CRPC_TEST(sqlstate_syntax_and_access_errors_are_invalid_arguments) {
    // 42xxx: syntax error, missing table, permission denied. The statement or the grant is
    // wrong; retrying the same statement cannot help.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(classify("42000")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(classify("42S02")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(classify("42S22")));
}

CRPC_TEST(sqlstate_deadlock_is_retryable) {
    // 40xxx: deadlock or serialisation failure. The transaction was rolled back and
    // re-running it is the documented remedy.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(classify("40001")));
}

CRPC_TEST(sqlstate_driver_timeout_is_a_deadline) {
    // HYT00/HYT01 are the driver's own timeout states; reporting them as a deadline lets a
    // caller distinguish "slow" from "broken".
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(classify("HYT00")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(classify("HYT01")));
}

CRPC_TEST(sqlstate_authorization_is_a_precondition) {
    // 28xxx: invalid authorization. A wrong password is not retryable as-is.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(classify("28000")));
}

CRPC_TEST(sqlstate_data_exception_is_an_invalid_argument) {
    // 22xxx: a value does not fit its column, division by zero, invalid datetime.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(classify("22001")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(classify("22012")));
}

CRPC_TEST(sqlstate_unknown_and_malformed_are_application_errors) {
    // An unknown class is the driver's own business; reporting it as `application` keeps it
    // distinguishable from a transport failure.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(classify("HY000")));
    // A state shorter than two characters cannot be classified.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(classify("")));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(classify("2")));
}

CRPC_TEST(odbc_bridge_reports_whether_it_is_available) {
    // The answer depends on the build, but it must be answerable without connecting: a
    // caller decides whether to use the bridge before it has a connection string.
    const bool present = crpc::db::odbc::available();
    CRPC_CHECK(present == true || present == false);
    // Without the bridge, connecting must fail with a clear precondition rather than a
    // transport error that looks like a network problem.
    if (!present) {
        crpc::db::odbc::connection_options options;
        options.connection_string = "Driver={does not matter};";
        crpc::db::odbc::connection session(options);
        CRPC_CHECK_FALSE(session.is_open());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(session.connect_error().code));
    }
}

CRPC_TEST_MAIN()