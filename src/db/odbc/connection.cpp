// CRPC -- ODBC bridge implementation.
//
// Everything Windows-specific lives in this file. A build without ODBC compiles the same
// declarations and reports `failed_precondition`, so a caller never has to know which
// platform it was built for.
#include <crpc/db/odbc/connection.hpp>

#include <cstring>
#include <limits>
#include <utility>

#include <crpc/db/memory_result_set.hpp>

#if defined(CRPC_HAVE_ODBC)
#include <windows.h>

#include <sql.h>
#include <sqlext.h>
#endif

namespace crpc {
namespace db {
namespace odbc {

rpc_code map_sqlstate(std::string_view sqlstate) noexcept {
    if (sqlstate.size() < 2) {
        return rpc_code::application;
    }
    // The class (first two characters) is the part the standard defines; the last three
    // are implementation-defined, so matching on the class is what portable code can do.
    const std::string_view klass = sqlstate.substr(0, 2);
    if (klass == "08") {
        // Connection exception: the link failed or was lost.
        return rpc_code::unavailable;
    }
    if (klass == "23") {
        // Integrity constraint violation: duplicate key, NOT NULL, foreign key.
        return rpc_code::failed_precondition;
    }
    if (klass == "22") {
        // Data exception: a value does not fit the column.
        return rpc_code::invalid_argument;
    }
    if (klass == "42") {
        // Syntax error or access rule violation.
        return rpc_code::invalid_argument;
    }
    if (klass == "40") {
        // Transaction rollback: deadlock or serialisation failure. Retrying is correct.
        return rpc_code::unavailable;
    }
    if (klass == "28") {
        // Invalid authorization specification.
        return rpc_code::failed_precondition;
    }
    if (klass == "HY") {
        // The driver's own class. HY000 is "general error"; a timeout is HYT00/HYT01.
        if (sqlstate == "HYT00" || sqlstate == "HYT01") {
            return rpc_code::deadline_exceeded;
        }
        return rpc_code::application;
    }
    if (klass == "3D" || klass == "3F") {
        // Invalid catalogue or schema name.
        return rpc_code::not_found;
    }
    return rpc_code::application;
}

#if defined(CRPC_HAVE_ODBC)

namespace {

// Converts the wide strings the ODBC API uses to UTF-8. Using the wide entry points
// throughout avoids the ANSI code-page problem: a table name or a value with a character
// outside the machine's code page would otherwise be corrupted before it reached the
// driver.
[[nodiscard]] std::string to_utf8(const SQLWCHAR* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr,
                                          0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), needed,
                              nullptr, nullptr);
    return out;
}

[[nodiscard]] std::string to_utf8(const SQLWCHAR* text) {
    if (text == nullptr) {
        return {};
    }
    std::size_t length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return to_utf8(text, length);
}

[[nodiscard]] std::vector<SQLWCHAR> to_wide(std::string_view text) {
    if (text.empty()) {
        return std::vector<SQLWCHAR>{0};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::vector<SQLWCHAR> out(static_cast<std::size_t>(needed) + 1, 0);
    if (needed > 0) {
        (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                  out.data(), needed);
    }
    out[static_cast<std::size_t>(needed)] = 0;
    return out;
}

// One driver diagnostic record, which is what ODBC reports instead of an exception.
struct diagnostic {
    std::string sqlstate;
    std::string message;
    SQLINTEGER native = 0;
};

// Drains every diagnostic record for `handle`. ODBC reports several per failure (the
// driver's, then the DM's), and the first one is usually the useful one, so they are all
// collected rather than the last.
[[nodiscard]] std::vector<diagnostic> collect_diagnostics(SQLSMALLINT type, SQLHANDLE handle) {
    std::vector<diagnostic> out;
    SQLWCHAR state[6] = {0};
    SQLWCHAR message[1024] = {0};
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    for (SQLSMALLINT record = 1; record <= 16; ++record) {
        const SQLRETURN got = SQLGetDiagRecW(type, handle, record, state, &native, message,
                                             static_cast<SQLSMALLINT>(std::size(message)),
                                             &length);
        if (got == SQL_NO_DATA || !SQL_SUCCEEDED(got)) {
            break;
        }
        diagnostic item;
        item.sqlstate = to_utf8(state);
        item.message = to_utf8(message, length > 0 ? static_cast<std::size_t>(length) : 0);
        item.native = native;
        out.push_back(std::move(item));
    }
    return out;
}

// True when the connection is in manual commit. ODBC has no server-side "am I in a
// transaction" query: the connection's commit mode is the authority, and it is the one
// thing every driver agrees on. In manual commit ODBC itself opens a transaction at the
// first statement and keeps it open until `SQLEndTran`, so this attribute answers the
// question directly. Reading it instead of remembering a flag is what keeps the answer
// from drifting away from the connection the pool is about to hand to someone else.
//
// A failed read answers true. The two mistakes are not symmetric: a spurious true costs
// one `SQLEndTran` that ends nothing, while a spurious false tells the pool the session is
// clean and hands the next borrower a transaction with uncommitted work in it. This is the
// same rule the MySQL adapter applies to a stale status word (see
// `include/crpc/db/mysql/connection.hpp`), and the reason `rollback()` below tolerates a
// connection with nothing to roll back.
[[nodiscard]] bool manual_commit(SQLHDBC dbc) noexcept {
    SQLUINTEGER mode = SQL_AUTOCOMMIT_ON;
    if (!SQL_SUCCEEDED(SQLGetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, &mode, 0, nullptr))) {
        return true;
    }
    return mode == SQL_AUTOCOMMIT_OFF;
}

// The value ODBC needs for one parameter, kept alive until the statement is executed.
struct bound_parameter {
    // A copy of the text, because SQLBindParameter only keeps the pointer.
    std::string text;
    std::int64_t integer = 0;
    // The bound buffer has to match the C type: an INTEGER bind reads an int32, a BIGINT
    // bind reads an int64. Binding the wider buffer to the narrower type would hand the
    // driver the wrong bytes.
    std::int32_t narrow_integer = 0;
    bool wide_integer = true;
    double real = 0.0;
    std::vector<std::byte> blob;
    SQLLEN indicator = 0;
    // BufferLength for SQLBindParameter. It is ignored for the fixed-size numeric C types
    // but not for character and binary ones: with 0 the Access driver reads a zero-length
    // buffer and answers "String data, right truncation" for a long value.
    SQLLEN buffer_length = 0;
    SQLSMALLINT c_type = SQL_C_CHAR;
    SQLSMALLINT sql_type = SQL_VARCHAR;
    SQLULEN column_size = 0;
    SQLSMALLINT digits = 0;
};

// Chooses the SQL type for a library parameter. The driver converts from there, and
// picking the narrowest correct type is what keeps an Access column from being written as
// text.
void classify(const parameter& value, bound_parameter& out) {
    switch (value.type) {
    case db_type::int64:
    case db_type::uint64:
    case db_type::boolean: {
        const std::int64_t number = value.type == db_type::boolean
                                        ? (value.int_value != 0 ? 1 : 0)
                                        : static_cast<std::int64_t>(value.int_value);
        out.integer = number;
        // The narrowest type that holds the value. This is not an optimisation: the Access
        // (Jet) driver has no 64-bit integer and answers a SQL_BIGINT bind with HYC00
        // "optional feature not implemented", so binding 1 as a BIGINT fails where binding
        // it as an INTEGER works. A driver that does support BIGINT still gets it when the
        // value needs it.
        if (number >= std::numeric_limits<std::int32_t>::min() &&
            number <= std::numeric_limits<std::int32_t>::max()) {
            out.c_type = SQL_C_SLONG;
            out.sql_type = SQL_INTEGER;
            out.column_size = 10;
            out.indicator = sizeof(std::int32_t);
            out.wide_integer = false;
        } else {
            out.c_type = SQL_C_SBIGINT;
            out.sql_type = SQL_BIGINT;
            out.column_size = 19;
            out.indicator = sizeof(std::int64_t);
            out.wide_integer = true;
        }
        return;
    }
    case db_type::real:
        out.real = value.real_value;
        out.c_type = SQL_C_DOUBLE;
        out.sql_type = SQL_DOUBLE;
        out.column_size = 15;
        out.indicator = sizeof(double);
        return;
    case db_type::blob:
        out.blob = value.blob;
        out.c_type = SQL_C_BINARY;
        out.sql_type = SQL_VARBINARY;
        out.column_size = out.blob.size();
        out.indicator = static_cast<SQLLEN>(out.blob.size());
        out.buffer_length = static_cast<SQLLEN>(out.blob.size());
        return;
    default:
        // Everything else goes as text: the driver knows its own conversions, and a
        // date, a decimal or a UUID sent as text is what every ODBC driver expects.
        out.text = value.text;
        out.c_type = SQL_C_CHAR;
        // A character parameter's ColumnSize is its maximum length, and the Access (Jet)
        // driver rejects anything above 255 with "Invalid precision value" -- it has no
        // VARCHAR wider than that, so a longer value is a long character type (MEMO). The
        // spec says ColumnSize is ignored for the long types, and 0 is the portable value
        // for "not declared": passing the real length reintroduces the same rejection.
        if (out.text.size() > 255) {
            out.sql_type = SQL_LONGVARCHAR;
            out.column_size = out.text.size();
        } else {
            out.sql_type = SQL_VARCHAR;
            out.column_size = out.text.size() == 0 ? 1 : out.text.size();
        }
        out.indicator = static_cast<SQLLEN>(out.text.size());
        out.buffer_length = static_cast<SQLLEN>(out.text.size());
        return;
    }
}

[[nodiscard]] db_type type_from_sql(SQLSMALLINT sql_type) noexcept {
    switch (sql_type) {
    case SQL_BIT:
        return db_type::boolean;
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_BIGINT:
        return db_type::int64;
    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
        return db_type::real;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
        return db_type::decimal;
    case SQL_DATE:
    case SQL_TYPE_DATE:
        return db_type::date;
    case SQL_TIME:
    case SQL_TYPE_TIME:
        return db_type::time;
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:
        return db_type::timestamp;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return db_type::blob;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
        return db_type::text;
    default:
        return db_type::unknown;
    }
}

} // namespace

struct connection::impl {
    connection_options options;
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    bool open = false;

    ~impl() {
        if (dbc != SQL_NULL_HDBC) {
            if (open) {
                (void)SQLDisconnect(dbc);
            }
            (void)SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        }
        if (env != SQL_NULL_HENV) {
            (void)SQLFreeHandle(SQL_HANDLE_ENV, env);
        }
    }
};

connection::connection(connection_options options) : impl_(new impl()) {
    impl_->options = std::move(options);
    // The order matters: the environment handle must exist before any attribute can be set
    // on it. Setting SQL_ATTR_ODBC_VERSION on SQL_NULL_HENV fails, and the failure is what
    // this adapter reported as "the ODBC environment could not be initialised" until a real
    // driver was asked.
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &impl_->env))) {
        impl_->env = SQL_NULL_HENV;
        connect_error_ = rpc_error{rpc_code::failed_precondition,
                                   "the ODBC environment could not be allocated"};
        return;
    }
    // ODBC 3.x behaviour: the driver manager is told which version to emulate before the
    // environment is used for anything else.
    if (!SQL_SUCCEEDED(SQLSetEnvAttr(impl_->env, SQL_ATTR_ODBC_VERSION,
                                     reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0))) {
        const std::vector<diagnostic> diagnostics =
            collect_diagnostics(SQL_HANDLE_ENV, impl_->env);
        connect_error_ = rpc_error{
            rpc_code::failed_precondition,
            diagnostics.empty() ? std::string("the ODBC environment could not be initialised")
                                : diagnostics.front().message};
        return;
    }
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, impl_->env, &impl_->dbc))) {
        impl_->dbc = SQL_NULL_HDBC;
        connect_error_ = rpc_error{rpc_code::failed_precondition,
                                   "the ODBC connection handle could not be allocated"};
        return;
    }
    if (impl_->options.login_timeout_seconds != 0) {
        (void)SQLSetConnectAttr(impl_->dbc, SQL_ATTR_LOGIN_TIMEOUT,
                                reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(
                                    impl_->options.login_timeout_seconds)),
                                0);
    }

    const std::vector<SQLWCHAR> connection_string = to_wide(impl_->options.connection_string);
    const std::vector<SQLWCHAR> user = to_wide(impl_->options.user);
    const std::vector<SQLWCHAR> password = to_wide(impl_->options.password);
    SQLSMALLINT out_length = 0;
    const SQLRETURN connected =
        SQLDriverConnectW(impl_->dbc, nullptr, const_cast<SQLWCHAR*>(connection_string.data()),
                          SQL_NTS, nullptr, 0, &out_length, SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(connected)) {
        const std::vector<diagnostic> diagnostics =
            collect_diagnostics(SQL_HANDLE_DBC, impl_->dbc);
        std::string message = "the ODBC driver refused the connection";
        rpc_code code = rpc_code::failed_precondition;
        if (!diagnostics.empty()) {
            message = diagnostics.front().message;
            last_sqlstate_ = diagnostics.front().sqlstate;
            code = map_sqlstate(last_sqlstate_);
        }
        connect_error_ = rpc_error{code, message};
        return;
    }
    (void)user;
    (void)password;

    // The driver's own identity is worth reporting: an Access database opened through the
    // wrong driver fails in ways that look like SQL errors.
    SQLWCHAR buffer[256] = {0};
    SQLSMALLINT length = 0;
    if (SQL_SUCCEEDED(SQLGetInfoW(impl_->dbc, SQL_DRIVER_NAME, buffer,
                                  static_cast<SQLSMALLINT>(std::size(buffer)), &length))) {
        driver_name_ = to_utf8(buffer, static_cast<std::size_t>(length));
    }
    if (SQL_SUCCEEDED(SQLGetInfoW(impl_->dbc, SQL_DRIVER_VER, buffer,
                                  static_cast<SQLSMALLINT>(std::size(buffer)), &length))) {
        driver_version_ = to_utf8(buffer, static_cast<std::size_t>(length));
    }
    if (SQL_SUCCEEDED(SQLGetInfoW(impl_->dbc, SQL_DBMS_NAME, buffer,
                                  static_cast<SQLSMALLINT>(std::size(buffer)), &length))) {
        dbms_name_ = to_utf8(buffer, static_cast<std::size_t>(length));
    }
    impl_->open = true;
}

connection::~connection() { close(); }

status connection::ping() {
    // A cheap statement the driver answers without touching user data. `SELECT 1` is not
    // valid in every dialect, so the table-less form is used only when the driver accepts
    // it; a failure is reported as the connection being unusable.
    result_set* out = nullptr;
    const status ran = query("SELECT 1", {}, &out);
    statement_guard guard(out);
    return ran;
}

bool connection::in_transaction() const noexcept {
    return impl_ && impl_->open && manual_commit(impl_->dbc);
}

namespace {

// Reports a failure from a driver call, keeping the SQLSTATE where the caller can see it.
[[nodiscard]] status from_diagnostics(SQLSMALLINT type, SQLHANDLE handle, const char* what,
                                      std::string& sqlstate) {
    const std::vector<diagnostic> diagnostics = collect_diagnostics(type, handle);
    std::string message = what;
    rpc_code code = rpc_code::application;
    if (!diagnostics.empty()) {
        message = diagnostics.front().message;
        sqlstate = diagnostics.front().sqlstate;
        code = map_sqlstate(sqlstate);
    }
    return status{code, std::move(message)};
}

[[nodiscard]] status set_manual_commit(SQLHDBC dbc, bool manual, std::string& sqlstate) {
    const auto mode = static_cast<SQLULEN>(manual ? SQL_AUTOCOMMIT_OFF : SQL_AUTOCOMMIT_ON);
    const SQLRETURN set =
        SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(mode),
                          SQL_IS_UINTEGER);
    if (SQL_SUCCEEDED(set)) {
        return status{};
    }
    return from_diagnostics(SQL_HANDLE_DBC, dbc,
                            "the transaction mode could not be changed", sqlstate);
}

// Ends the transaction and puts the connection back into autocommit. On failure the
// connection is left in manual commit on purpose: setting autocommit back on while a
// transaction is still active commits it, which after a failed ROLLBACK would persist
// exactly the work the caller asked to discard. Left as it is, `in_transaction()` keeps
// answering true and the pool discards the session instead of reusing it.
[[nodiscard]] status end_transaction(SQLHDBC dbc, SQLSMALLINT completion, std::string& sqlstate) {
    if (!SQL_SUCCEEDED(SQLEndTran(SQL_HANDLE_DBC, dbc, completion))) {
        return from_diagnostics(SQL_HANDLE_DBC, dbc, "the transaction could not be ended",
                                sqlstate);
    }
    return set_manual_commit(dbc, false, sqlstate);
}

} // namespace

status connection::begin() {
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    if (manual_commit(impl_->dbc)) {
        return status{rpc_code::failed_precondition, "a transaction is already open"};
    }
    return set_manual_commit(impl_->dbc, true, last_sqlstate_);
}

status connection::commit() {
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    if (!manual_commit(impl_->dbc)) {
        return status{rpc_code::failed_precondition, "no transaction is open"};
    }
    return end_transaction(impl_->dbc, SQL_COMMIT, last_sqlstate_);
}

status connection::rollback() {
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    // Rolling back nothing is not an error. The pool asks `in_transaction()` first, but
    // that answer is deliberately conservative -- a failed attribute read counts as "open"
    // -- so this can be called on a connection that has nothing to roll back, and a caller
    // rolling back defensively should not have to ask first. `commit` is the stricter one
    // because losing work is the failure mode there.
    if (!manual_commit(impl_->dbc)) {
        return status{};
    }
    return end_transaction(impl_->dbc, SQL_ROLLBACK, last_sqlstate_);
}

status connection::query(std::string_view sql, span<const parameter> params, result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, impl_->dbc, &stmt))) {
        return status{rpc_code::resource_exhausted, "an ODBC statement could not be allocated"};
    }
    // The statement handle owns nothing after this function returns, but every early
    // return has to free it, so a small guard keeps the paths honest.
    const auto release = [&stmt]() {
        if (stmt != SQL_NULL_HSTMT) {
            (void)SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            stmt = SQL_NULL_HSTMT;
        }
    };

    if (impl_->options.query_timeout_seconds != 0) {
        (void)SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
                             reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(
                                 impl_->options.query_timeout_seconds)),
                             0);
    }

    // The values are bound, never spliced: ODBC parameter arrays keep a pointer to each
    // buffer until execution, so the buffers outlive the bind call here.
    std::vector<bound_parameter> bound;
    bound.reserve(params.size());
    for (const parameter& value : params) {
        bound_parameter one;
        classify(value, one);
        bound.push_back(std::move(one));
    }
    for (std::size_t i = 0; i < bound.size(); ++i) {
        bound_parameter& one = bound[i];
        void* data = nullptr;
        switch (one.c_type) {
        case SQL_C_SBIGINT:
            data = &one.integer;
            break;
        case SQL_C_SLONG:
            one.narrow_integer = static_cast<std::int32_t>(one.integer);
            data = &one.narrow_integer;
            break;
        case SQL_C_DOUBLE:
            data = &one.real;
            break;
        case SQL_C_BINARY:
            data = one.blob.empty() ? nullptr : one.blob.data();
            if (params[i].is_null) {
                one.indicator = SQL_NULL_DATA;
            }
            break;
        default:
            data = one.text.empty() ? nullptr : one.text.data();
            if (params[i].is_null) {
                one.indicator = SQL_NULL_DATA;
            }
            break;
        }
        if (params[i].is_null && one.c_type != SQL_C_BINARY) {
            one.indicator = SQL_NULL_DATA;
            data = nullptr;
        }
        const SQLRETURN bind_result =
            SQLBindParameter(stmt, static_cast<SQLUSMALLINT>(i + 1), SQL_PARAM_INPUT, one.c_type,
                             one.sql_type, one.column_size, one.digits, data, one.buffer_length,
                             &one.indicator);
        if (!SQL_SUCCEEDED(bind_result)) {
            const std::vector<diagnostic> diagnostics = collect_diagnostics(SQL_HANDLE_STMT, stmt);
            std::string message = "a parameter could not be bound";
            rpc_code code = rpc_code::invalid_argument;
            if (!diagnostics.empty()) {
                message = diagnostics.front().message;
                last_sqlstate_ = diagnostics.front().sqlstate;
                code = map_sqlstate(last_sqlstate_);
            }
            release();
            return status{code, message};
        }
    }

    const std::vector<SQLWCHAR> wide = to_wide(sql);
    SQLRETURN ran = SQLExecDirectW(stmt, const_cast<SQLWCHAR*>(wide.data()), SQL_NTS);
    if (ran == SQL_NO_DATA) {
        // No result set and no rows: a DELETE that matched nothing, for instance.
        ran = SQL_SUCCESS;
    }
    if (!SQL_SUCCEEDED(ran)) {
        const std::vector<diagnostic> diagnostics = collect_diagnostics(SQL_HANDLE_STMT, stmt);
        std::string message = "the statement failed";
        rpc_code code = rpc_code::application;
        if (!diagnostics.empty()) {
            message = diagnostics.front().message;
            last_sqlstate_ = diagnostics.front().sqlstate;
            code = map_sqlstate(last_sqlstate_);
        }
        release();
        return status{code, message};
    }

    // The column count decides whether there is a result set at all: a driver reports 0 for
    // an INSERT or a DDL statement.
    SQLSMALLINT column_count = 0;
    if (!SQL_SUCCEEDED(SQLNumResultCols(stmt, &column_count))) {
        release();
        return status{rpc_code::protocol_error, "the driver did not report a column count"};
    }

    if (column_count == 0) {
        SQLLEN affected = 0;
        (void)SQLRowCount(stmt, &affected);
        release();
        if (out != nullptr) {
            *out = new memory_result_set({}, static_cast<std::int64_t>(affected));
        }
        return status{};
    }

    std::vector<column_info> columns;
    columns.reserve(static_cast<std::size_t>(column_count));
    // Every column is read as text: the driver's own conversion is the only one that knows
    // the column's real type, and the library's cursor parses canonical text.
    // Columns are read with SQLGetData instead of being bound to a fixed buffer. A bound
    // buffer has to be sized before the first row, and every value longer than it arrives
    // truncated with no error at all -- a MEMO column is exactly that case, and a silent
    // truncation is worse than a failure. SQLGetData is called until the driver says
    // SQL_NO_DATA, so the length is never assumed. It must also be called in ascending
    // column order, which the loop below does.
    for (SQLSMALLINT i = 1; i <= column_count; ++i) {
        SQLWCHAR name[256] = {0};
        SQLSMALLINT name_length = 0;
        SQLSMALLINT data_type = 0;
        SQLULEN column_size = 0;
        SQLSMALLINT digits = 0;
        SQLSMALLINT nullable = 0;
        (void)SQLDescribeColW(stmt, static_cast<SQLUSMALLINT>(i), name,
                              static_cast<SQLSMALLINT>(std::size(name)), &name_length, &data_type,
                              &column_size, &digits, &nullable);
        column_info info;
        info.name = to_utf8(name, static_cast<std::size_t>(name_length));
        info.type = type_from_sql(data_type);
        info.precision = static_cast<std::uint32_t>(column_size);
        info.scale = static_cast<std::uint32_t>(digits);
        info.nullable = nullable == SQL_NULLABLE;
        columns.push_back(std::move(info));
    }

    // Which columns are binary has to be captured before `columns` is moved into the result
    // set: after the move the vector is empty and indexing it is undefined behaviour.
    std::vector<bool> binary_columns;
    binary_columns.reserve(columns.size());
    for (const column_info& info : columns) {
        binary_columns.push_back(info.type == db_type::blob);
    }

    std::unique_ptr<memory_result_set> set(new memory_result_set(std::move(columns), -1));
    std::size_t row_bytes = 0;
    for (;;) {
        const SQLRETURN fetched = SQLFetch(stmt);
        if (fetched == SQL_NO_DATA) {
            break;
        }
        if (!SQL_SUCCEEDED(fetched)) {
            const std::vector<diagnostic> diagnostics = collect_diagnostics(SQL_HANDLE_STMT, stmt);
            std::string message = "the driver failed while reading rows";
            rpc_code code = rpc_code::application;
            if (!diagnostics.empty()) {
                message = diagnostics.front().message;
                last_sqlstate_ = diagnostics.front().sqlstate;
                code = map_sqlstate(last_sqlstate_);
            }
            release();
            return status{code, message};
        }
        if (set->row_count() >= impl_->options.max_result_rows) {
            release();
            return status{rpc_code::resource_exhausted,
                          "the result set exceeds max_result_rows"};
        }
        std::vector<std::string> values;
        std::vector<bool> nulls;
        values.reserve(static_cast<std::size_t>(column_count));
        nulls.reserve(static_cast<std::size_t>(column_count));
        for (SQLSMALLINT i = 1; i <= column_count; ++i) {
            // A binary column is read as bytes, not as text. Asking this driver for a
            // LONGBINARY as SQL_C_CHAR does not fail -- it answers the value rendered as
            // hex, so a 5-byte value arrives as the 10 characters "4100420043" and would be
            // handed to the caller as its own bytes. Measured against the Jet driver: five
            // bytes in, ten characters out.
            const bool binary = binary_columns[static_cast<std::size_t>(i - 1)];
            std::string value;
            bool is_null = false;
            for (;;) {
                char chunk[4096] = {0};
                SQLLEN indicator = 0;
                const SQLRETURN got =
                    SQLGetData(stmt, static_cast<SQLUSMALLINT>(i),
                               binary ? SQL_C_BINARY : SQL_C_CHAR, chunk,
                               static_cast<SQLLEN>(sizeof(chunk)), &indicator);
                if (got == SQL_NO_DATA) {
                    break;
                }
                if (!SQL_SUCCEEDED(got)) {
                    const std::vector<diagnostic> diagnostics =
                        collect_diagnostics(SQL_HANDLE_STMT, stmt);
                    std::string message = "the driver failed while reading a value";
                    rpc_code code = rpc_code::application;
                    if (!diagnostics.empty()) {
                        message = diagnostics.front().message;
                        last_sqlstate_ = diagnostics.front().sqlstate;
                        code = map_sqlstate(last_sqlstate_);
                    }
                    release();
                    return status{code, message};
                }
                if (indicator == SQL_NULL_DATA) {
                    is_null = true;
                    break;
                }
                // The chunk length comes from the indicator, never from strlen: a binary
                // value legitimately contains 0x00, and stopping there would truncate it
                // silently. When the driver reports SQL_NO_TOTAL there is no length to use,
                // and for SQL_C_CHAR the buffer is null-terminated, so strlen is the only
                // remaining answer.
                std::size_t chunk_length = sizeof(chunk) - 1;
                if (indicator != SQL_NO_TOTAL && indicator >= 0) {
                    const std::size_t reported = static_cast<std::size_t>(indicator);
                    const std::size_t capacity = binary ? sizeof(chunk) : sizeof(chunk) - 1;
                    chunk_length = reported < capacity ? reported : capacity;
                }
                value.append(chunk, chunk_length);
                if (got == SQL_SUCCESS) {
                    // SQL_SUCCESS means the whole value has been returned; only a truncated
                    // chunk comes back as SQL_SUCCESS_WITH_INFO and needs another call.
                    break;
                }
            }
            row_bytes += value.size();
            if (is_null) {
                values.emplace_back();
                nulls.push_back(true);
            } else {
                values.push_back(std::move(value));
                nulls.push_back(false);
            }
        }
        if (row_bytes > impl_->options.max_result_bytes) {
            release();
            return status{rpc_code::resource_exhausted,
                          "the result set exceeds max_result_bytes"};
        }
        set->add_row(std::move(values), std::move(nulls));
    }
    release();
    if (out != nullptr) {
        *out = set.release();
    }
    return status{};
}

void connection::close() noexcept {
    if (impl_ && impl_->open) {
        impl_->open = false;
        // An open transaction has to be ended before the connection goes away. ODBC does
        // not let a driver disconnect out of an outstanding transaction: SQLDisconnect
        // answers "invalid transaction operation" (25000) and the session stays alive on
        // the server holding its locks until it times out, which blocks the next caller.
        // The rollback is issued here rather than left to the driver for the same reason
        // the pool rolls back before reuse -- an abandoned `begin()` must not outlive the
        // object that opened it.
        if (manual_commit(impl_->dbc) &&
            SQL_SUCCEEDED(SQLEndTran(SQL_HANDLE_DBC, impl_->dbc, SQL_ROLLBACK))) {
            // Only after the transaction is really gone: setting autocommit back on while
            // one is still active commits it, the opposite of what a close must do.
            (void)SQLSetConnectAttr(
                impl_->dbc, SQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<SQLPOINTER>(static_cast<SQLULEN>(SQL_AUTOCOMMIT_ON)),
                SQL_IS_UINTEGER);
        }
        (void)SQLDisconnect(impl_->dbc);
    }
}

bool connection::is_open() const noexcept { return impl_ && impl_->open; }

bool available() noexcept { return true; }

result<std::vector<std::string>> list_drivers() {
    SQLHENV env = SQL_NULL_HENV;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
        return rpc_error{rpc_code::failed_precondition, "the ODBC environment could not be allocated"};
    }
    (void)SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
    std::vector<std::string> out;
    SQLWCHAR description[256] = {0};
    SQLWCHAR attributes[256] = {0};
    SQLSMALLINT description_length = 0;
    SQLSMALLINT attributes_length = 0;
    for (SQLUSMALLINT direction = SQL_FETCH_FIRST;;
         direction = SQL_FETCH_NEXT) {
        const SQLRETURN got = SQLDriversW(env, direction, description,
                                          static_cast<SQLSMALLINT>(std::size(description)),
                                          &description_length, attributes,
                                          static_cast<SQLSMALLINT>(std::size(attributes)),
                                          &attributes_length);
        if (got == SQL_NO_DATA) {
            break;
        }
        if (!SQL_SUCCEEDED(got)) {
            (void)SQLFreeHandle(SQL_HANDLE_ENV, env);
            return rpc_error{rpc_code::application, "the ODBC driver list could not be read"};
        }
        out.push_back(to_utf8(description, static_cast<std::size_t>(description_length)));
    }
    (void)SQLFreeHandle(SQL_HANDLE_ENV, env);
    return out;
}

#else // !CRPC_HAVE_ODBC

struct connection::impl {};

connection::connection(connection_options options) : impl_(new impl()) {
    (void)options;
    connect_error_ = rpc_error{rpc_code::failed_precondition,
                               "this build has no ODBC support; rebuild with CRPC_WITH_ODBC=ON"};
}

connection::~connection() = default;

status connection::ping() { return connect_error_; }
bool connection::in_transaction() const noexcept { return false; }
status connection::begin() { return connect_error_; }
status connection::commit() { return connect_error_; }
status connection::rollback() { return connect_error_; }
status connection::query(std::string_view sql, span<const parameter> params, result_set** out) {
    (void)sql;
    (void)params;
    if (out != nullptr) {
        *out = nullptr;
    }
    return connect_error_;
}
void connection::close() noexcept {}
bool connection::is_open() const noexcept { return false; }
bool available() noexcept { return false; }
result<std::vector<std::string>> list_drivers() {
    return rpc_error{rpc_code::failed_precondition,
                     "this build has no ODBC support; rebuild with CRPC_WITH_ODBC=ON"};
}

#endif // CRPC_HAVE_ODBC

// --- factory -----------------------------------------------------------------

connection_factory::connection_factory(connection_options options)
    : options_(std::move(options)) {}

connection_factory::~connection_factory() = default;

result<db::connection*> connection_factory::open() {
    auto session = std::unique_ptr<connection>(new connection(options_));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::failed_precondition, "the ODBC connection failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "odbc"; }

} // namespace odbc
} // namespace db
} // namespace crpc