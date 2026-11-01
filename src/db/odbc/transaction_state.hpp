// CRPC -- the decision manual_commit() makes, separated from the driver call.
//
// The rule itself is one line and easy to get backwards, which is exactly what happened
// once: `SQLGetConnectAttr` failing was read as "not in a transaction", and the pool
// handed sessions with uncommitted work to the next borrower. Keeping the truth table in
// a pure function is what lets a test pin the direction without an ODBC driver to make the
// read fail.
#ifndef CRPC_SRC_DB_ODBC_TRANSACTION_STATE_HPP
#define CRPC_SRC_DB_ODBC_TRANSACTION_STATE_HPP

namespace crpc {
namespace db {
namespace odbc {
namespace detail {

// True when the connection should be reported as having an open transaction.
//
// `read_ok` is false when the driver refused to report its commit mode. The two mistakes
// are not symmetric: claiming a transaction that is not there costs one `SQLEndTran` that
// ends nothing, while denying one that is there tells the pool the session is clean and
// hands on uncommitted work. So an unreadable state is reported as open.
[[nodiscard]] constexpr bool transaction_state_from_attr(bool read_ok,
                                                         bool autocommit_off) noexcept {
    return !read_ok || autocommit_off;
}

} // namespace detail
} // namespace odbc
} // namespace db
} // namespace crpc

#endif // CRPC_SRC_DB_ODBC_TRANSACTION_STATE_HPP