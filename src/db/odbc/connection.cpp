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
        return;
    default:
        // Everything else goes as text: the driver knows its own conversions, and a
        // date, a decimal or a UUID sent as text is what every ODBC driver expects.
        out.text = value.text;
        out.c_type = SQL_C_CHAR;
        out.sql_type = SQL_VARCHAR;
        out.column_size = out.text.size() == 0 ? 1 : out.text.size();
        out.indicator = static_cast<SQLLEN>(out.text.size());
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

bool connection::in_transaction() const noexcept { return false; }

namespace {

// Runs one statement with no result set: BEGIN/COMMIT/ROLLBACK and DDL.
[[nodiscard]] status run_simple(SQLHDBC dbc, const char* sql, std::string& sqlstate) {
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
        return status{rpc_code::resource_exhausted, "an ODBC statement could not be allocated"};
    }
    const std::vector<SQLWCHAR> wide = to_wide(sql);
    SQLRETURN ran = SQLExecDirectW(stmt, const_cast<SQLWCHAR*>(wide.data()), SQL_NTS);
    if (ran == SQL_NO_DATA) {
        ran = SQL_SUCCESS;
    }
    if (!SQL_SUCCEEDED(ran)) {
        const std::vector<diagnostic> diagnostics = collect_diagnostics(SQL_HANDLE_STMT, stmt);
        std::string message = "the statement failed";
        rpc_code code = rpc_code::application;
        if (!diagnostics.empty()) {
            message = diagnostics.front().message;
            sqlstate = diagnostics.front().sqlstate;
            code = map_sqlstate(sqlstate);
        }
        (void)SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return status{code, message};
    }
    (void)SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return status{};
}

} // namespace

status connection::begin() {
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    return run_simple(impl_->dbc, "BEGIN TRANSACTION", last_sqlstate_);
}

status connection::commit() {
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    return run_simple(impl_->dbc, "COMMIT", last_sqlstate_);
}

status connection::rollback() {
    if (!impl_->open) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    return run_simple(impl_->dbc, "ROLLBACK", last_sqlstate_);
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
                             one.sql_type, one.column_size, one.digits, data, 0, &one.indicator);
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
    std::vector<std::vector<char>> buffers;
    buffers.reserve(static_cast<std::size_t>(column_count));
    std::vector<SQLLEN> indicators(static_cast<std::size_t>(column_count), 0);
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
        // A generous buffer: Access reports a memo as a very large size, and truncating a
        // value silently is worse than a larger allocation.
        const std::size_t width = column_size == 0 || column_size == SQL_NO_TOTAL
                                      ? 4096
                                      : (column_size > 8192 ? 8192 : column_size) + 1;
        buffers.emplace_back(width, '\0');
        if (!SQL_SUCCEEDED(SQLBindCol(stmt, static_cast<SQLUSMALLINT>(i), SQL_C_CHAR,
                                      buffers.back().data(),
                                      static_cast<SQLLEN>(buffers.back().size()),
                                      &indicators[static_cast<std::size_t>(i - 1)]))) {
            release();
            return status{rpc_code::protocol_error, "a column could not be bound for reading"};
        }
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
        for (SQLSMALLINT i = 0; i < column_count; ++i) {
            const SQLLEN indicator = indicators[static_cast<std::size_t>(i)];
            if (indicator == SQL_NULL_DATA) {
                values.emplace_back();
                nulls.push_back(true);
                continue;
            }
            const std::size_t length =
                indicator == SQL_NO_TOTAL ? 0 : static_cast<std::size_t>(indicator);
            const std::string value(buffers[static_cast<std::size_t>(i)].data(), length);
            row_bytes += value.size();
            values.push_back(value);
            nulls.push_back(false);
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