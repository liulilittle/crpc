// CRPC -- SQLite backend implementation.
//
// The engine is called through its C API only. There is no SQL rewriting, no string
// escaping and no ORM in this file: a caller's text is prepared verbatim, and every value
// travels through `sqlite3_bind_*`.
//
// Linking model
// -------------
// The engine is resolved at run time: `winsqlite3.dll` on Windows (the SQLite build that
// ships with the Windows SDK) and `libsqlite3.so`/`.dylib` elsewhere. Two things follow:
//
//   * adding this backend needs no build-system change, which keeps a build without
//     SQLite compiling unchanged (the header is detected with __has_include);
//   * `available()` answers reality. A machine without the shared library reports false
//     instead of failing to start at dynamic-loader time.
//
// When the header is absent, or the library cannot be loaded, everything below compiles
// to the "unavailable" implementation: `available()` is false and a connection reports
// `failed_precondition` with the reason.
#include <crpc/db/sqlite/connection.hpp>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(CRPC_HAVE_SQLITE3)
// An explicit build definition wins; otherwise the presence of a header decides whether
// the backend exists in this build. This is the guard the build system would set.
#if defined(__has_include)
#if defined(_WIN32)
#if __has_include(<winsqlite/winsqlite3.h>)
#define CRPC_HAVE_SQLITE3 1
#endif
#elif __has_include(<sqlite3.h>) && __has_include(<dlfcn.h>)
#define CRPC_HAVE_SQLITE3 1
#endif
#endif
#endif

#if defined(CRPC_HAVE_SQLITE3)
#if defined(CRPC_SQLITE_USE_SYSTEM_HEADER)
#include <sqlite3.h>
#elif defined(_WIN32)
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif // CRPC_HAVE_SQLITE3

#include "../text_value.hpp"

namespace crpc {
namespace db {
namespace sqlite {

#if defined(CRPC_HAVE_SQLITE3)

namespace {

// --- dynamic symbol table ----------------------------------------------------
//
// Every entry is typed by `decltype(&::sqlite3_x)`, so the calling convention and the
// parameter types come from the header rather than from a hand-written typedef. A field
// is null only on the failure path where `loaded` is already false.

struct api_table {
    decltype(&::sqlite3_open_v2) open_v2 = nullptr;
    decltype(&::sqlite3_close_v2) close_v2 = nullptr;
    decltype(&::sqlite3_errmsg) errmsg = nullptr;
    decltype(&::sqlite3_errcode) errcode = nullptr;
    decltype(&::sqlite3_extended_errcode) extended_errcode = nullptr;
    decltype(&::sqlite3_extended_result_codes) extended_result_codes = nullptr;
    decltype(&::sqlite3_busy_timeout) busy_timeout = nullptr;
    decltype(&::sqlite3_prepare_v2) prepare_v2 = nullptr;
    decltype(&::sqlite3_step) step = nullptr;
    decltype(&::sqlite3_finalize) finalize = nullptr;
    decltype(&::sqlite3_bind_parameter_count) bind_parameter_count = nullptr;
    decltype(&::sqlite3_bind_null) bind_null = nullptr;
    decltype(&::sqlite3_bind_int64) bind_int64 = nullptr;
    decltype(&::sqlite3_bind_double) bind_double = nullptr;
    decltype(&::sqlite3_bind_text) bind_text = nullptr;
    decltype(&::sqlite3_bind_blob) bind_blob = nullptr;
    decltype(&::sqlite3_column_count) column_count = nullptr;
    decltype(&::sqlite3_column_type) column_type = nullptr;
    decltype(&::sqlite3_column_int64) column_int64 = nullptr;
    decltype(&::sqlite3_column_double) column_double = nullptr;
    decltype(&::sqlite3_column_text) column_text = nullptr;
    decltype(&::sqlite3_column_bytes) column_bytes = nullptr;
    decltype(&::sqlite3_column_blob) column_blob = nullptr;
    decltype(&::sqlite3_column_name) column_name = nullptr;
    decltype(&::sqlite3_column_decltype) column_decltype = nullptr;
    decltype(&::sqlite3_changes) changes = nullptr;
    decltype(&::sqlite3_total_changes) total_changes = nullptr;
    decltype(&::sqlite3_get_autocommit) get_autocommit = nullptr;
    decltype(&::sqlite3_progress_handler) progress_handler = nullptr;
    decltype(&::sqlite3_libversion) libversion = nullptr;
    decltype(&::sqlite3_libversion_number) libversion_number = nullptr;
};

// The oldest engine this adapter supports. `sqlite3_close_v2` arrived in 3.7.14, and a
// WAL database (the default for a file) needs 3.7.0.
constexpr int minimum_version_number = 3007014;

struct backend_state {
    api_table fn;
    bool loaded = false;
    std::string error;
    std::string version = "unavailable";
};

#if defined(_WIN32)

// `GetProcAddress` returns a function pointer; converting it through `void*` with memcpy
// keeps the conversion out of the "unsafe conversion" diagnostic and is the documented
// way to call a resolved symbol.
template <typename T>
[[nodiscard]] T resolve(HMODULE module, const char* name) noexcept {
    FARPROC found = ::GetProcAddress(module, name);
    void* address = nullptr;
    static_assert(sizeof(address) == sizeof(found), "function and object pointers differ");
    std::memcpy(&address, &found, sizeof(address));
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "unexpected function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

[[nodiscard]] HMODULE load_library() noexcept {
    return ::LoadLibraryW(L"winsqlite3.dll");
}

[[nodiscard]] std::string load_error_text() noexcept {
    return std::string("winsqlite3.dll could not be loaded from the system directory");
}

#else

template <typename T>
[[nodiscard]] T resolve(void* handle, const char* name) noexcept {
    void* address = ::dlsym(handle, name);
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "unexpected function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

[[nodiscard]] void* load_library() noexcept {
    // The versioned soname is what a runtime-only installation provides; the unversioned
    // name is the development symlink. Neither is loaded with RTLD_GLOBAL: this library
    // must not leak sqlite3 symbols into a consumer's own SQLite plugin.
    void* handle = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        handle = ::dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    }
#if defined(__APPLE__)
    if (handle == nullptr) {
        handle = ::dlopen("libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
    }
#endif
    return handle;
}

[[nodiscard]] std::string load_error_text() noexcept {
    const char* reason = ::dlerror();
    if (reason == nullptr) {
        return std::string("libsqlite3 could not be loaded");
    }
    return std::string("libsqlite3 could not be loaded: ") + reason;
}

#endif

// True when every field that the adapter calls unconditionally was resolved.
[[nodiscard]] bool resolve_all(const api_table& fn) noexcept {
    return fn.open_v2 != nullptr && fn.close_v2 != nullptr && fn.errmsg != nullptr &&
           fn.errcode != nullptr && fn.extended_errcode != nullptr &&
           fn.extended_result_codes != nullptr && fn.busy_timeout != nullptr &&
           fn.prepare_v2 != nullptr && fn.step != nullptr && fn.finalize != nullptr &&
           fn.bind_parameter_count != nullptr && fn.bind_null != nullptr &&
           fn.bind_int64 != nullptr && fn.bind_double != nullptr && fn.bind_text != nullptr &&
           fn.bind_blob != nullptr && fn.column_count != nullptr &&
           fn.column_type != nullptr && fn.column_int64 != nullptr &&
           fn.column_double != nullptr && fn.column_text != nullptr &&
           fn.column_bytes != nullptr && fn.column_blob != nullptr &&
           fn.column_name != nullptr && fn.column_decltype != nullptr &&
           fn.changes != nullptr && fn.total_changes != nullptr &&
           fn.get_autocommit != nullptr && fn.progress_handler != nullptr &&
           fn.libversion != nullptr && fn.libversion_number != nullptr;
}

[[nodiscard]] backend_state load_backend() noexcept {
    backend_state state;
    const auto library = load_library();
    if (library == nullptr) {
        state.error = load_error_text();
        return state;
    }
    api_table fn;
    fn.open_v2 = resolve<decltype(fn.open_v2)>(library, "sqlite3_open_v2");
    fn.close_v2 = resolve<decltype(fn.close_v2)>(library, "sqlite3_close_v2");
    fn.errmsg = resolve<decltype(fn.errmsg)>(library, "sqlite3_errmsg");
    fn.errcode = resolve<decltype(fn.errcode)>(library, "sqlite3_errcode");
    fn.extended_errcode =
        resolve<decltype(fn.extended_errcode)>(library, "sqlite3_extended_errcode");
    fn.extended_result_codes = resolve<decltype(fn.extended_result_codes)>(
        library, "sqlite3_extended_result_codes");
    fn.busy_timeout = resolve<decltype(fn.busy_timeout)>(library, "sqlite3_busy_timeout");
    fn.prepare_v2 = resolve<decltype(fn.prepare_v2)>(library, "sqlite3_prepare_v2");
    fn.step = resolve<decltype(fn.step)>(library, "sqlite3_step");
    fn.finalize = resolve<decltype(fn.finalize)>(library, "sqlite3_finalize");
    fn.bind_parameter_count = resolve<decltype(fn.bind_parameter_count)>(
        library, "sqlite3_bind_parameter_count");
    fn.bind_null = resolve<decltype(fn.bind_null)>(library, "sqlite3_bind_null");
    fn.bind_int64 = resolve<decltype(fn.bind_int64)>(library, "sqlite3_bind_int64");
    fn.bind_double = resolve<decltype(fn.bind_double)>(library, "sqlite3_bind_double");
    fn.bind_text = resolve<decltype(fn.bind_text)>(library, "sqlite3_bind_text");
    fn.bind_blob = resolve<decltype(fn.bind_blob)>(library, "sqlite3_bind_blob");
    fn.column_count = resolve<decltype(fn.column_count)>(library, "sqlite3_column_count");
    fn.column_type = resolve<decltype(fn.column_type)>(library, "sqlite3_column_type");
    fn.column_int64 = resolve<decltype(fn.column_int64)>(library, "sqlite3_column_int64");
    fn.column_double = resolve<decltype(fn.column_double)>(library, "sqlite3_column_double");
    fn.column_text = resolve<decltype(fn.column_text)>(library, "sqlite3_column_text");
    fn.column_bytes = resolve<decltype(fn.column_bytes)>(library, "sqlite3_column_bytes");
    fn.column_blob = resolve<decltype(fn.column_blob)>(library, "sqlite3_column_blob");
    fn.column_name = resolve<decltype(fn.column_name)>(library, "sqlite3_column_name");
    fn.column_decltype =
        resolve<decltype(fn.column_decltype)>(library, "sqlite3_column_decltype");
    fn.changes = resolve<decltype(fn.changes)>(library, "sqlite3_changes");
    fn.total_changes = resolve<decltype(fn.total_changes)>(library, "sqlite3_total_changes");
    fn.get_autocommit =
        resolve<decltype(fn.get_autocommit)>(library, "sqlite3_get_autocommit");
    fn.progress_handler =
        resolve<decltype(fn.progress_handler)>(library, "sqlite3_progress_handler");
    fn.libversion = resolve<decltype(fn.libversion)>(library, "sqlite3_libversion");
    fn.libversion_number =
        resolve<decltype(fn.libversion_number)>(library, "sqlite3_libversion_number");

    if (!resolve_all(fn)) {
        state.error = "the sqlite3 library is missing a required entry point";
        return state;
    }
    if (fn.libversion_number() < minimum_version_number) {
        state.error = "the sqlite3 library is too old (3.7.14 or newer is required)";
        return state;
    }
    state.fn = fn;
    const char* version = fn.libversion();
    state.version = version != nullptr ? version : "unknown";
    state.loaded = true;
    return state;
}

// A function-local static: initialisation is thread-safe since C++11 and happens once, so
// two threads racing to open the first connection both wait for the same load.
[[nodiscard]] const backend_state& backend() noexcept {
    static const backend_state state = load_backend();
    return state;
}

[[nodiscard]] const api_table& api() noexcept { return backend().fn; }

// --- errors ------------------------------------------------------------------

[[nodiscard]] std::string to_string_int(int value) { return std::to_string(value); }

// Builds the error value for the connection's current failure. The primary code is the
// low byte: with extended result codes enabled `sqlite3_errcode` hands back the extended
// value, so masking recovers the portable code the mapping is written against.
[[nodiscard]] rpc_error failure(sqlite3* db, rpc_code code) {
    const int extended = db != nullptr ? api().extended_errcode(db) : 0;
    rpc_error error{
        code, db != nullptr ? std::string(api().errmsg(db))
                            : std::string("the sqlite3 handle is not open")};
    error.metadata.set("sqlite_code", to_string_int(extended & 0xff));
    error.metadata.set("sqlite_extended_code", to_string_int(extended));
    return error;
}

// The current failure of `db`, classified from the engine's own code.
[[nodiscard]] rpc_error failure_from_db(sqlite3* db) {
    const int extended = db != nullptr ? api().extended_errcode(db) : 0;
    return failure(db, map_errcode(extended));
}

// Runs one statement whose rows are irrelevant (BEGIN/COMMIT/ROLLBACK/PRAGMA). The
// transaction verbs must not go through `query()`: that path builds a cursor, and a caller
// would have to delete a cursor it never wanted.
[[nodiscard]] status run_control(sqlite3* db, std::string_view sql) {
    if (db == nullptr) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    sqlite3_stmt* stmt = nullptr;
    const int prepared = api().prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt,
                                         nullptr);
    if (prepared != SQLITE_OK) {
        return status{failure_from_db(db)};
    }
    if (stmt == nullptr) {
        return status{rpc_code::internal, "the control statement prepared to nothing"};
    }
    int stepped = api().step(stmt);
    if (stepped != SQLITE_ROW && stepped != SQLITE_DONE) {
        status result{failure_from_db(db)};
        (void)api().finalize(stmt);
        return result;
    }
    // Drain whatever rows a PRAGMA returns; the length is not known in advance.
    while (stepped == SQLITE_ROW) {
        stepped = api().step(stmt);
    }
    if (stepped != SQLITE_DONE) {
        status result{failure_from_db(db)};
        (void)api().finalize(stmt);
        return result;
    }
    const int finalized = api().finalize(stmt);
    if (finalized != SQLITE_OK) {
        return status{failure_from_db(db)};
    }
    return status{};
}

// Reads the first column of the first row, for `PRAGMA` answers that are one value.
[[nodiscard]] std::string read_pragma_text(sqlite3* db, std::string_view pragma) {
    sqlite3_stmt* stmt = nullptr;
    const int prepared = api().prepare_v2(db, pragma.data(), static_cast<int>(pragma.size()),
                                         &stmt, nullptr);
    if (prepared != SQLITE_OK) {
        (void)api().finalize(stmt);
        return {};
    }
    std::string out;
    if (api().step(stmt) == SQLITE_ROW) {
        const unsigned char* text = api().column_text(stmt, 0);
        const int length = api().column_bytes(stmt, 0);
        if (text != nullptr && length > 0) {
            out.assign(reinterpret_cast<const char*>(text), static_cast<std::size_t>(length));
        }
    }
    (void)api().finalize(stmt);
    return out;
}

// --- declared type mapping ---------------------------------------------------

[[nodiscard]] std::string upper_of(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool starts_with(const std::string& text, const char* prefix) noexcept {
    const std::size_t length = std::strlen(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

// Maps SQLite's declared type to a library type. SQLite's own affinity rules decide
// INT/CHAR/CLOB/TEXT/BLOB/REAL/FLOA/DOUB; the remaining names are handled because the
// declared type is the only hint a column carries, and a caller mapping an ORM entity
// needs BOOLEAN, DATETIME, DECIMAL and friends to keep their meaning.
[[nodiscard]] db_type type_from_declared(std::string_view declared) {
    if (declared.empty()) {
        // An expression column (SELECT 1 + 1) has no declared type. Reporting a guess
        // would be wrong for at least one caller, so the type stays unknown.
        return db_type::unknown;
    }
    const std::string name = upper_of(declared);
    if (starts_with(name, "BOOL")) {
        return db_type::boolean;
    }
    if (starts_with(name, "INT")) {
        return db_type::int64;
    }
    if (starts_with(name, "REAL") || starts_with(name, "FLOA") || starts_with(name, "DOUB")) {
        return db_type::real;
    }
    // DATETIME before DATE and TIMESTAMP before TIME: each is a prefix of the other.
    if (starts_with(name, "DATETIME") || starts_with(name, "TIMESTAMP")) {
        return db_type::timestamp;
    }
    if (starts_with(name, "DATE")) {
        return db_type::date;
    }
    if (starts_with(name, "TIME")) {
        return db_type::time;
    }
    if (starts_with(name, "DECIMAL") || starts_with(name, "NUMERIC")) {
        return db_type::decimal;
    }
    if (starts_with(name, "JSON")) {
        return db_type::json;
    }
    if (starts_with(name, "UUID")) {
        return db_type::uuid;
    }
    if (starts_with(name, "XML")) {
        return db_type::xml;
    }
    if (starts_with(name, "CHAR") || starts_with(name, "VARCHAR") ||
        starts_with(name, "NCHAR") || starts_with(name, "NVARCHAR") ||
        starts_with(name, "CLOB") || starts_with(name, "TEXT")) {
        return db_type::text;
    }
    if (starts_with(name, "BLOB")) {
        return db_type::blob;
    }
    return db_type::unknown;
}

// Reads `(precision[, scale])` out of a declared type such as DECIMAL(10,2). A declaration
// without parentheses reports zero, which is the struct's "not declared" value.
void parse_precision(const std::string& declared, std::uint32_t& precision,
                     std::uint32_t& scale) noexcept {
    const std::size_t open = declared.find('(');
    if (open == std::string::npos) {
        return;
    }
    const std::size_t close = declared.find(')', open + 1);
    if (close == std::string::npos) {
        return;
    }
    std::uint32_t values[2] = {0, 0};
    std::size_t index = open + 1;
    int slot = 0;
    bool any_digit = false;
    for (; index < close && slot < 2; ++index) {
        const char c = declared[index];
        if (c >= '0' && c <= '9') {
            const auto digit = static_cast<std::uint32_t>(c - '0');
            if (values[slot] > (std::numeric_limits<std::uint32_t>::max() - digit) / 10u) {
                values[slot] = std::numeric_limits<std::uint32_t>::max();
            } else {
                values[slot] = values[slot] * 10u + digit;
            }
            any_digit = true;
        } else if (c == ',') {
            if (!any_digit) {
                return;
            }
            ++slot;
            any_digit = false;
        } else if (c == ' ' || c == '\t') {
            continue;
        } else {
            return;
        }
    }
    precision = values[0];
    scale = values[1];
}

// --- cells -------------------------------------------------------------------

// One decoded value. The storage class is kept explicitly so an accessor can tell a
// number from text that happens to spell it.
struct cell {
    int storage = SQLITE_NULL;
    bool is_null = true;
    std::int64_t integer = 0;
    double real = 0.0;
    // TEXT and BLOB both live here; the size is the string's own, so an embedded NUL is
    // preserved exactly as the engine returned it.
    std::string bytes;
};

[[nodiscard]] cell read_cell(sqlite3_stmt* stmt, int index) {
    cell out;
    out.storage = api().column_type(stmt, index);
    switch (out.storage) {
    case SQLITE_INTEGER:
        out.is_null = false;
        out.integer = api().column_int64(stmt, index);
        break;
    case SQLITE_FLOAT:
        out.is_null = false;
        out.real = api().column_double(stmt, index);
        break;
    case SQLITE_TEXT: {
        out.is_null = false;
        // Text before bytes: the SQLite documentation requires that order, and bytes then
        // reports the length of the converted UTF-8, not a NUL scan.
        const auto* text = api().column_text(stmt, index);
        const int length = api().column_bytes(stmt, index);
        if (text != nullptr && length > 0) {
            out.bytes.assign(reinterpret_cast<const char*>(text),
                             static_cast<std::size_t>(length));
        }
        break;
    }
    case SQLITE_BLOB: {
        out.is_null = false;
        const void* data = api().column_blob(stmt, index);
        const int length = api().column_bytes(stmt, index);
        if (data != nullptr && length > 0) {
            out.bytes.assign(static_cast<const char*>(data), static_cast<std::size_t>(length));
        }
        break;
    }
    case SQLITE_NULL:
    default:
        out.is_null = true;
        break;
    }
    return out;
}

// --- row accessors -----------------------------------------------------------

[[nodiscard]] result<std::int64_t> cell_int64(const cell& value) {
    if (value.is_null) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    switch (value.storage) {
    case SQLITE_INTEGER:
        return value.integer;
    case SQLITE_TEXT:
        return detail::parse_int64_text(value.bytes);
    default:
        return rpc_error{rpc_code::invalid_argument, "value is not an integer"};
    }
}

[[nodiscard]] result<std::uint64_t> cell_uint64(const cell& value) {
    if (value.is_null) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    switch (value.storage) {
    case SQLITE_INTEGER:
        if (value.integer < 0) {
            return rpc_error{rpc_code::invalid_argument,
                             "a negative value cannot be read as uint64"};
        }
        return static_cast<std::uint64_t>(value.integer);
    case SQLITE_TEXT:
        return detail::parse_uint64_text(value.bytes);
    default:
        return rpc_error{rpc_code::invalid_argument, "value is not an integer"};
    }
}

[[nodiscard]] result<double> cell_double(const cell& value) {
    if (value.is_null) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    switch (value.storage) {
    case SQLITE_INTEGER:
        return static_cast<double>(value.integer);
    case SQLITE_FLOAT:
        return value.real;
    case SQLITE_TEXT:
        return detail::parse_double_text(value.bytes);
    default:
        return rpc_error{rpc_code::invalid_argument, "value is not a number"};
    }
}

[[nodiscard]] result<bool> cell_bool(const cell& value) {
    if (value.is_null) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    switch (value.storage) {
    case SQLITE_INTEGER:
        if (value.integer == 0) {
            return false;
        }
        if (value.integer == 1) {
            return true;
        }
        return rpc_error{rpc_code::decode_error, "value is not a boolean"};
    case SQLITE_TEXT:
        return detail::parse_bool_text(value.bytes);
    default:
        return rpc_error{rpc_code::invalid_argument, "value is not a boolean"};
    }
}

// Integers and reals are rendered into `scratch` for `get_text`. The integer conversion is
// exact; the real conversion uses the same 15 significant digits SQLite's own text
// conversion produces, so the caller does not have to reimplement the formatting.
[[nodiscard]] result<std::string_view> cell_text(const cell& value, std::string& scratch) {
    if (value.is_null) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    switch (value.storage) {
    case SQLITE_TEXT:
        return std::string_view(value.bytes);
    case SQLITE_INTEGER:
        scratch = std::to_string(value.integer);
        return std::string_view(scratch);
    case SQLITE_FLOAT: {
        char buffer[64] = {0};
        const int written = std::snprintf(buffer, sizeof(buffer), "%.15g", value.real);
        scratch.assign(buffer, written > 0 ? static_cast<std::size_t>(written) : 0);
        return std::string_view(scratch);
    }
    default:
        return rpc_error{rpc_code::invalid_argument, "value is not text"};
    }
}

[[nodiscard]] result<std::vector<std::byte>> cell_blob(const cell& value) {
    if (value.is_null) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    if (value.storage != SQLITE_BLOB) {
        return rpc_error{rpc_code::invalid_argument, "value is not a blob"};
    }
    std::vector<std::byte> out(value.bytes.size());
    if (!value.bytes.empty()) {
        std::memcpy(out.data(), value.bytes.data(), value.bytes.size());
    }
    return out;
}

// --- handle ------------------------------------------------------------------

// The engine handle plus everything that must outlive a `connection` object: a streaming
// cursor holds a `shared_ptr` to this, so a cursor still being read is never left with a
// freed database. `close()` marks the handle unusable and closes the engine handle once the
// last cursor has released it.
struct handle {
    sqlite3* db = nullptr;
    bool closed = false;
    int cursors = 0;

    std::chrono::milliseconds timeout{0};
    std::chrono::steady_clock::time_point deadline{};
    bool armed = false;
    bool timed_out = false;

    ~handle() {
        if (db != nullptr) {
            (void)api().close_v2(db);
            db = nullptr;
        }
    }

    handle() = default;
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;

    // Called by a streaming cursor when it is finished. The real close is deferred until
    // the last cursor is gone; `sqlite3_close_v2` would also accept open statements, but
    // it leaves the database unusable for them, and a cursor the caller still holds would
    // then fail with a less useful error than its rows ending.
    void release_cursor() noexcept {
        if (cursors > 0) {
            --cursors;
        }
        if (closed && cursors == 0 && db != nullptr) {
            (void)api().close_v2(db);
            db = nullptr;
        }
    }
};

// sqlite3_progress_handler's callback. Returning non-zero aborts the running statement
// with SQLITE_INTERRUPT; `timed_out` is what distinguishes our deadline from any other
// interruption.
int SQLITE_CALLBACK on_progress(void* user) noexcept {
    auto* state = static_cast<handle*>(user);
    if (state == nullptr || !state->armed) {
        return 0;
    }
    if (std::chrono::steady_clock::now() >= state->deadline) {
        state->timed_out = true;
        return 1;
    }
    return 0;
}

// Starts the deadline for one statement. A stale deadline from an earlier statement must
// not abort this one, so the state is reset first.
void arm_timeout(handle& h) noexcept {
    h.timed_out = false;
    h.armed = h.timeout.count() > 0;
    if (h.armed) {
        h.deadline = std::chrono::steady_clock::now() + h.timeout;
    }
}

void disarm_timeout(handle& h) noexcept {
    h.armed = false;
    h.timed_out = false;
}

// --- errors for a step -------------------------------------------------------

// The error a failed `sqlite3_step` reports. A deadline hit by the progress handler comes
// back as SQLITE_INTERRUPT, which is otherwise just a cancel; the flag decides.
[[nodiscard]] rpc_error step_failure(sqlite3* db, const handle& h) {
    if (h.timed_out) {
        const int extended = db != nullptr ? api().extended_errcode(db) : SQLITE_INTERRUPT;
        rpc_error error{rpc_code::deadline_exceeded, "the command timeout elapsed"};
        error.metadata.set("sqlite_code", to_string_int(extended & 0xff));
        error.metadata.set("sqlite_extended_code", to_string_int(extended));
        return error;
    }
    return failure_from_db(db);
}

// --- statement preparation ---------------------------------------------------

// True when the rest of a SQL string is only whitespace or semicolons. `sqlite3_prepare_v2`
// stops at the first statement and would silently ignore anything after it; a caller that
// sent two statements must hear about it instead.
[[nodiscard]] bool only_whitespace_or_semicolons(std::string_view tail) noexcept {
    for (const char c : tail) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] status bind_parameters(sqlite3_stmt* stmt, span<const parameter> params) {
    const int expected = api().bind_parameter_count(stmt);
    if (static_cast<std::size_t>(expected) != params.size()) {
        return status{rpc_code::invalid_argument,
                      "the statement expects " + std::to_string(expected) +
                          " parameter(s) but " + std::to_string(params.size()) +
                          " were supplied"};
    }
    // A zero-length blob needs a non-null pointer: `sqlite3_bind_blob` treats a null data
    // pointer as SQL NULL regardless of the length.
    static const char empty_blob = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const parameter& value = params[i];
        const int index = static_cast<int>(i + 1);
        int bound = SQLITE_OK;
        if (value.is_null) {
            bound = api().bind_null(stmt, index);
        } else {
            switch (value.type) {
            case db_type::boolean:
                bound = api().bind_int64(stmt, index, value.int_value != 0 ? 1 : 0);
                break;
            case db_type::int64:
                bound = api().bind_int64(stmt, index,
                                         static_cast<sqlite3_int64>(value.int_value));
                break;
            case db_type::uint64:
                if (value.uint_value >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    // SQLite's INTEGER is signed 64-bit. Wrapping would hand the engine a
                    // negative number the caller never passed, so the value is refused.
                    return status{rpc_code::invalid_argument,
                                  "value exceeds the signed 64-bit range SQLite stores"};
                }
                bound = api().bind_int64(stmt, index,
                                         static_cast<sqlite3_int64>(value.uint_value));
                break;
            case db_type::real:
                bound = api().bind_double(stmt, index, value.real_value);
                break;
            case db_type::blob:
                bound = api().bind_blob(stmt, index,
                                        value.blob.empty()
                                            ? static_cast<const void*>(&empty_blob)
                                            : value.blob.data(),
                                        static_cast<int>(value.blob.size()),
                                        SQLITE_TRANSIENT);
                break;
            default:
                // Every textual type (text, decimal, date, time, timestamp, uuid, json,
                // xml) travels as UTF-8 text, and the explicit length keeps an embedded
                // NUL. `unknown` lands here too: the other backends treat it as text, and
                // binding a number the caller never declared as numeric would be worse.
                bound = api().bind_text(stmt, index, value.text.data(),
                                        static_cast<int>(value.text.size()), SQLITE_TRANSIENT);
                break;
            }
        }
        if (bound != SQLITE_OK) {
            rpc_code code = map_errcode(bound);
            rpc_error error{code, std::string("parameter ") + std::to_string(index) +
                                      " could not be bound"};
            error.metadata.set("sqlite_code", to_string_int(bound & 0xff));
            error.metadata.set("sqlite_extended_code", to_string_int(bound));
            return status{std::move(error)};
        }
    }
    return status{};
}

// Prepares exactly one statement and binds `params`. On failure `*out` stays null and the
// returned status is the caller's answer.
[[nodiscard]] status prepare_statement(sqlite3* db, std::string_view sql,
                                       span<const parameter> params, sqlite3_stmt** out) {
    *out = nullptr;
    if (sql.empty()) {
        return status{rpc_code::invalid_argument, "the SQL text is empty"};
    }
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return status{rpc_code::invalid_argument, "the SQL text is too long"};
    }
    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;
    const int prepared =
        api().prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, &tail);
    if (prepared != SQLITE_OK) {
        (void)api().finalize(stmt);
        return status{failure_from_db(db)};
    }
    if (stmt == nullptr) {
        // A string of only comments or semicolons prepares to nothing.
        return status{rpc_code::invalid_argument, "the SQL text contains no statement"};
    }
    if (tail != nullptr) {
        const char* end = sql.data() + sql.size();
        if (tail < sql.data() || tail > end ||
            !only_whitespace_or_semicolons(
                std::string_view(tail, static_cast<std::size_t>(end - tail)))) {
            (void)api().finalize(stmt);
            return status{rpc_code::invalid_argument,
                          "only one SQL statement is accepted per call"};
        }
    }
    const status bound = bind_parameters(stmt, params);
    if (!bound.ok()) {
        (void)api().finalize(stmt);
        return bound;
    }
    *out = stmt;
    return status{};
}

// --- column descriptions -----------------------------------------------------

[[nodiscard]] std::vector<column_info> describe_columns(sqlite3_stmt* stmt) {
    const int count = api().column_count(stmt);
    std::vector<column_info> columns;
    if (count <= 0) {
        return columns;
    }
    columns.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        column_info info;
        const char* name = api().column_name(stmt, i);
        if (name != nullptr) {
            info.name = name;
        }
        const char* declared = api().column_decltype(stmt, i);
        const std::string declared_type = declared != nullptr ? std::string(declared) : std::string();
        info.type = type_from_declared(declared_type);
        parse_precision(declared_type, info.precision, info.scale);
        // SQLite does not expose a column's nullability through this API, and a value can
        // be NULL regardless of a NOT NULL declaration on an outer join or an expression
        // column. Reporting "may be null" is the honest answer.
        info.nullable = true;
        columns.push_back(std::move(info));
    }
    return columns;
}

// --- cursors -----------------------------------------------------------------

// The accessor implementation both cursors share. Where the current row comes from is the
// only difference between them, so the type rules live here once.
class sqlite_cursor_base : public result_set {
public:
    std::size_t column_count() const noexcept override { return columns_.size(); }

    column_info column(std::size_t index) const override {
        return index < columns_.size() ? columns_[index] : column_info{};
    }

    bool is_null(std::size_t index) const noexcept override {
        const cell* value = current_cell(index);
        return value == nullptr || value->is_null;
    }

    result<std::int64_t> get_int64(std::size_t index) override {
        const cell* value = current_cell(index);
        if (value == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return cell_int64(*value);
    }

    result<std::uint64_t> get_uint64(std::size_t index) override {
        const cell* value = current_cell(index);
        if (value == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return cell_uint64(*value);
    }

    result<double> get_double(std::size_t index) override {
        const cell* value = current_cell(index);
        if (value == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return cell_double(*value);
    }

    result<bool> get_bool(std::size_t index) override {
        const cell* value = current_cell(index);
        if (value == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return cell_bool(*value);
    }

    result<std::string_view> get_text(std::size_t index) override {
        const cell* value = current_cell(index);
        if (value == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return cell_text(*value, scratch_);
    }

    result<std::vector<std::byte>> get_blob(std::size_t index) override {
        const cell* value = current_cell(index);
        if (value == nullptr) {
            return rpc_error{rpc_code::invalid_argument, "column is NULL"};
        }
        return cell_blob(*value);
    }

    std::int64_t affected_rows() const noexcept override { return affected_; }

protected:
    [[nodiscard]] virtual const cell* current_cell(std::size_t index) const noexcept = 0;

    std::vector<column_info> columns_;
    std::int64_t affected_ = -1;
    // True when `next()` has produced a row that the accessors may read.
    bool has_row_ = false;
    // `get_text` may have to render an integer or a real; the rendered text lives here so
    // the returned view stays valid until the next accessor call.
    std::string scratch_;
};

// A materialised cursor: every row decoded before `query()` returns.
class materialised_cursor final : public sqlite_cursor_base {
public:
    materialised_cursor(std::vector<column_info> columns, std::int64_t affected) {
        columns_ = std::move(columns);
        affected_ = affected;
    }

    void add_row(std::vector<cell> row) { rows_.push_back(std::move(row)); }

    bool next() override {
        if (next_ >= rows_.size()) {
            has_row_ = false;
            return false;
        }
        cursor_ = next_++;
        has_row_ = true;
        return true;
    }

    void close() noexcept override {
        rows_.clear();
        rows_.shrink_to_fit();
        next_ = 0;
        cursor_ = 0;
        has_row_ = false;
    }

    std::size_t rows_decoded() const noexcept override { return rows_.size(); }
    std::size_t rows_buffered() const noexcept override { return rows_.size(); }

protected:
    const cell* current_cell(std::size_t index) const noexcept override {
        if (!has_row_ || cursor_ >= rows_.size() || index >= rows_[cursor_].size()) {
            return nullptr;
        }
        return &rows_[cursor_][index];
    }

private:
    std::vector<std::vector<cell>> rows_;
    std::size_t next_ = 0;
    std::size_t cursor_ = 0;
};

// A streaming cursor: one row decoded per `next()`, straight from the engine.
class streaming_cursor final : public sqlite_cursor_base {
public:
    streaming_cursor(std::shared_ptr<handle> state, sqlite3_stmt* stmt,
                     std::vector<column_info> columns)
        : state_(std::move(state)), stmt_(stmt) {
        columns_ = std::move(columns);
        row_.resize(columns_.size());
        state_->cursors += 1;
    }

    ~streaming_cursor() override { close(); }

    bool next() override {
        if (closed_ || done_) {
            has_row_ = false;
            return false;
        }
        if (state_->closed) {
            // The connection was closed while this cursor was held. The statement is
            // finalised below; there is no row to hand out any more.
            last_error_ = rpc_error{rpc_code::shutting_down, "the connection was closed"};
            fail();
            return false;
        }
        if (!stepping_) {
            stepping_ = true;
            arm_timeout(*state_);
        }
        const int stepped = api().step(stmt_);
        if (stepped == SQLITE_ROW) {
            for (int i = 0; i < static_cast<int>(row_.size()); ++i) {
                row_[static_cast<std::size_t>(i)] = read_cell(stmt_, i);
            }
            ++decoded_;
            has_row_ = true;
            return true;
        }
        has_row_ = false;
        if (stepped == SQLITE_DONE) {
            disarm_timeout(*state_);
            done_ = true;
            return false;
        }
        last_error_ = step_failure(state_->db, *state_);
        fail();
        return false;
    }

    bool is_streaming() const noexcept override { return true; }
    rpc_error last_error() const noexcept override { return last_error_; }
    std::size_t rows_decoded() const noexcept override { return decoded_; }
    std::size_t rows_buffered() const noexcept override { return has_row_ ? 1u : 0u; }

    void close() noexcept override {
        if (closed_) {
            return;
        }
        closed_ = true;
        has_row_ = false;
        done_ = true;
        if (stmt_ != nullptr) {
            // Finalising is what releases the engine's statement; a partially read result
            // is simply abandoned, which is what closing a cursor means.
            (void)api().finalize(stmt_);
            stmt_ = nullptr;
        }
        disarm_timeout(*state_);
        state_->release_cursor();
    }

protected:
    const cell* current_cell(std::size_t index) const noexcept override {
        if (!has_row_ || index >= row_.size()) {
            return nullptr;
        }
        return &row_[index];
    }

private:
    void fail() noexcept {
        has_row_ = false;
        done_ = true;
        disarm_timeout(*state_);
        if (stmt_ != nullptr) {
            (void)api().finalize(stmt_);
            stmt_ = nullptr;
        }
    }

    std::shared_ptr<handle> state_;
    sqlite3_stmt* stmt_ = nullptr;
    std::vector<cell> row_;
    rpc_error last_error_;
    std::size_t decoded_ = 0;
    bool stepping_ = false;
    bool done_ = false;
    bool closed_ = false;
};

// Runs a statement with no result columns to completion and reports the rows it changed.
// The count is the connection's `total_changes` before and after: `sqlite3_changes` would
// report the previous DML for a DDL statement, while the delta is zero for DDL and exact
// for INSERT/UPDATE/DELETE.
[[nodiscard]] status finish_without_columns(handle& h, sqlite3_stmt* stmt,
                                            std::int64_t& affected) {
    arm_timeout(h);
    const int before = api().total_changes(h.db);
    int stepped = api().step(stmt);
    while (stepped == SQLITE_ROW) {
        // A DML with RETURNING can still produce rows; they are not wanted here.
        stepped = api().step(stmt);
    }
    if (stepped != SQLITE_DONE) {
        status failed{step_failure(h.db, h)};
        disarm_timeout(h);
        (void)api().finalize(stmt);
        return failed;
    }
    const int after = api().total_changes(h.db);
    affected = static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before);
    disarm_timeout(h);
    const int finalized = api().finalize(stmt);
    if (finalized != SQLITE_OK) {
        return status{failure_from_db(h.db)};
    }
    return status{};
}

// Materialises a prepared statement into a cursor.
[[nodiscard]] status materialise(handle& h, sqlite3_stmt* stmt, std::size_t max_rows,
                                 result_set** out) {
    const int columns = api().column_count(stmt);
    std::vector<column_info> described = describe_columns(stmt);
    std::unique_ptr<materialised_cursor> cursor(
        new materialised_cursor(std::move(described), columns > 0 ? -1 : 0));

    arm_timeout(h);
    const int before = api().total_changes(h.db);
    std::size_t rows = 0;
    for (;;) {
        const int stepped = api().step(stmt);
        if (stepped == SQLITE_DONE) {
            break;
        }
        if (stepped != SQLITE_ROW) {
            status failed{step_failure(h.db, h)};
            disarm_timeout(h);
            (void)api().finalize(stmt);
            return failed;
        }
        if (rows >= max_rows) {
            disarm_timeout(h);
            (void)api().finalize(stmt);
            return status{rpc_code::resource_exhausted,
                          "the result set exceeds max_result_rows"};
        }
        std::vector<cell> row;
        row.reserve(static_cast<std::size_t>(columns));
        for (int i = 0; i < columns; ++i) {
            row.push_back(read_cell(stmt, i));
        }
        cursor->add_row(std::move(row));
        ++rows;
    }
    disarm_timeout(h);
    if (columns == 0) {
        const int after = api().total_changes(h.db);
        const int finalized = api().finalize(stmt);
        if (finalized != SQLITE_OK) {
            return status{failure_from_db(h.db)};
        }
        // A statement without columns is not a query: its cursor has no rows, only the
        // number of rows it changed.
        *out = new materialised_cursor(
            {}, static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before));
        return status{};
    }
    *out = cursor.release();
    const int finalized = api().finalize(stmt);
    if (finalized != SQLITE_OK) {
        delete *out;
        *out = nullptr;
        return status{failure_from_db(h.db)};
    }
    return status{};
}

} // namespace

#endif // CRPC_HAVE_SQLITE3

// --- result-code mapping -----------------------------------------------------
//
// The primary code is the low byte of an extended code, so an extended result such as
// SQLITE_CONSTRAINT_UNIQUE (2067) and its primary SQLITE_CONSTRAINT (19) answer the same
// way. The numbers are spelled as literals rather than through the header's macros so the
// mapping exists, and is tested, in a build that has no SQLite headers at all.
rpc_code map_errcode(int sqlite_errcode) noexcept {
    switch (sqlite_errcode & 0xff) {
    case 0: // SQLITE_OK
    case 100: // SQLITE_ROW
    case 101: // SQLITE_DONE
        return rpc_code::ok;
    case 1: // SQLITE_ERROR: a syntax error or a missing object
    case 20: // SQLITE_MISMATCH: a value has the wrong type for its column
    case 25: // SQLITE_RANGE: a bind index is out of range
        return rpc_code::invalid_argument;
    case 2: // SQLITE_INTERNAL
    case 10: // SQLITE_IOERR
    case 11: // SQLITE_CORRUPT
    case 15: // SQLITE_PROTOCOL
        return rpc_code::internal;
    case 3: // SQLITE_PERM
    case 8: // SQLITE_READONLY
    case 23: // SQLITE_AUTH
        return rpc_code::permission_denied;
    case 5: // SQLITE_BUSY
    case 6: // SQLITE_LOCKED
        return rpc_code::unavailable;
    case 7: // SQLITE_NOMEM
    case 13: // SQLITE_FULL
    case 18: // SQLITE_TOOBIG
        return rpc_code::resource_exhausted;
    case 9: // SQLITE_INTERRUPT
        return rpc_code::canceled;
    case 12: // SQLITE_NOTFOUND
    case 16: // SQLITE_EMPTY
        return rpc_code::not_found;
    case 14: // SQLITE_CANTOPEN
    case 22: // SQLITE_NOLFS: a file too large for the platform
    case 26: // SQLITE_NOTADB
        return rpc_code::failed_precondition;
    case 19: // SQLITE_CONSTRAINT (and every extended constraint code)
        return rpc_code::failed_precondition;
    case 21: // SQLITE_MISUSE: the API was called out of order
        return rpc_code::internal;
    case 4: // SQLITE_ABORT: a callback asked to abort, or a rollback happened
    case 17: // SQLITE_SCHEMA
    case 24: // SQLITE_FORMAT
    case 27: // SQLITE_NOTICE
    case 28: // SQLITE_WARNING
        return rpc_code::application;
    default:
        return rpc_code::application;
    }
}

// --- availability ------------------------------------------------------------

#if defined(CRPC_HAVE_SQLITE3)

bool available() noexcept { return backend().loaded; }

std::string_view version() noexcept { return backend().version; }

#else

bool available() noexcept { return false; }

std::string_view version() noexcept { return "unavailable"; }

#endif

// --- connection --------------------------------------------------------------

#if defined(CRPC_HAVE_SQLITE3)

struct connection::impl {
    std::shared_ptr<handle> state;
    // The engine's answer, read once after opening. WAL may be refused by the filesystem
    // (a network share), so the reported mode is what the engine settled on.
    std::string journal_mode;
    std::size_t max_result_rows = 1'000'000;
};

namespace {

[[nodiscard]] status open_engine(const connection_options& options,
                                 const std::shared_ptr<handle>& state, std::string& journal_mode,
                                 rpc_error& connect_error) {
    if (options.busy_timeout_ms < 0) {
        connect_error = rpc_error{rpc_code::invalid_argument,
                                  "busy_timeout_ms must not be negative"};
        return status{connect_error};
    }
    if (options.path.empty()) {
        connect_error = rpc_error{rpc_code::invalid_argument, "the database path is empty"};
        return status{connect_error};
    }

    // SQLITE_OPEN_NOMUTEX would let two threads use one connection at once; this adapter
    // guarantees no such thing, so the default (serialised) mode is the safe choice.
    int flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READWRITE;
    if (options.read_only) {
        flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_READONLY;
    } else if (options.create_if_missing) {
        flags |= SQLITE_OPEN_CREATE;
    }

    sqlite3* db = nullptr;
    const int opened =
        api().open_v2(options.path.c_str(), &db, flags, nullptr);
    if (opened != SQLITE_OK) {
        connect_error = failure(db, map_errcode(opened));
        if (db != nullptr) {
            (void)api().close_v2(db);
        }
        return status{connect_error};
    }
    state->db = db;
    // Extended codes are what the error mapping and the metadata are written against.
    (void)api().extended_result_codes(db, 1);
    if (options.busy_timeout_ms > 0) {
        (void)api().busy_timeout(db, options.busy_timeout_ms);
    }
    // A progress handler must be installed for `set_command_timeout` to have any effect
    // later; installing it once here avoids re-registering it per statement. The step count
    // is small on purpose: the handler is cheap (a steady_clock read) and a large count
    // would make a short timeout fire late.
    (void)api().progress_handler(db, 1000, &on_progress, state.get());

    const bool is_memory = options.path == ":memory:";
    if (!options.read_only && !is_memory && options.enable_wal) {
        const status applied = run_control(db, "PRAGMA journal_mode=WAL");
        if (!applied.ok()) {
            connect_error = applied.error();
            (void)api().close_v2(db);
            state->db = nullptr;
            return status{connect_error};
        }
    }
    journal_mode = read_pragma_text(db, "PRAGMA journal_mode");
    return status{};
}

} // namespace

connection::connection(connection_options options) : impl_(new impl()) {
    impl_->state = std::make_shared<handle>();
    impl_->max_result_rows = options.max_result_rows;
    const status opened = open_engine(options, impl_->state, impl_->journal_mode,
                                      connect_error_);
    if (!opened.ok()) {
        impl_->state.reset();
    }
}

connection::~connection() { close(); }

status connection::ping() {
    if (!is_open()) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    // `SELECT 1` is answered from the engine without touching a table.
    return run_control(impl_->state->db, "SELECT 1");
}

bool connection::in_transaction() const noexcept {
    if (!impl_ || !impl_->state || impl_->state->db == nullptr) {
        return false;
    }
    // The engine's own answer. A local flag would drift from a transaction started with
    // raw SQL, and the pool asks this before handing the session to someone else.
    return api().get_autocommit(impl_->state->db) == 0;
}

status connection::begin() {
    if (!is_open()) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    if (in_transaction()) {
        return status{rpc_code::failed_precondition, "a transaction is already open"};
    }
    return run_control(impl_->state->db, "BEGIN");
}

status connection::commit() {
    if (!is_open()) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    if (!in_transaction()) {
        return status{rpc_code::failed_precondition, "no transaction is open"};
    }
    const status ended = run_control(impl_->state->db, "COMMIT");
    if (!ended.ok()) {
        // A failed COMMIT leaves SQLite's transaction open in the cases that can be
        // retried (SQLITE_BUSY). The state is not forced here: `in_transaction()` keeps
        // reporting the truth, and the caller can roll back.
        return ended;
    }
    return ended;
}

status connection::rollback() {
    if (!is_open()) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    // Rolling back nothing is not an error: the pool calls this defensively after a failed
    // commit, and a caller should not have to ask first.
    if (!in_transaction()) {
        return status{};
    }
    return run_control(impl_->state->db, "ROLLBACK");
}

status connection::set_command_timeout(std::chrono::milliseconds timeout) noexcept {
    if (!impl_ || !impl_->state) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    const std::chrono::milliseconds effective =
        timeout.count() > 0 ? timeout : std::chrono::milliseconds{0};
    impl_->state->timeout = effective;
    // The base class copy is what `command_timeout()` reports; keeping the two in step is
    // the contract the base's default implementation already provides.
    command_timeout_ = effective;
    return status{};
}

status connection::query(std::string_view sql, span<const parameter> params,
                         result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!is_open()) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    sqlite3_stmt* stmt = nullptr;
    const status prepared = prepare_statement(impl_->state->db, sql, params, &stmt);
    if (!prepared.ok()) {
        return prepared;
    }
    return materialise(*impl_->state, stmt, impl_->max_result_rows, out);
}

status connection::query_streaming(std::string_view sql, span<const parameter> params,
                                   result_set** out) {
    if (out != nullptr) {
        *out = nullptr;
    }
    if (!is_open()) {
        return status{rpc_code::shutting_down, "the connection is closed"};
    }
    sqlite3_stmt* stmt = nullptr;
    const status prepared = prepare_statement(impl_->state->db, sql, params, &stmt);
    if (!prepared.ok()) {
        return prepared;
    }
    const int columns = api().column_count(stmt);
    if (columns <= 0) {
        // Nothing to stream: run it to completion and answer with the affected count, the
        // same shape `query()` produces for a DML.
        std::int64_t affected = -1;
        const status ran = finish_without_columns(*impl_->state, stmt, affected);
        if (!ran.ok()) {
            return ran;
        }
        if (out != nullptr) {
            *out = new materialised_cursor({}, affected);
        }
        return status{};
    }
    if (out != nullptr) {
        *out = new streaming_cursor(impl_->state, stmt, describe_columns(stmt));
    } else {
        (void)api().finalize(stmt);
    }
    return status{};
}

void connection::close() noexcept {
    if (!impl_ || !impl_->state) {
        return;
    }
    handle& state = *impl_->state;
    if (state.db == nullptr) {
        return;
    }
    // An open transaction is rolled back before the handle goes away. SQLite would roll it
    // back when the handle is closed anyway, but doing it explicitly keeps a pooled
    // connection's behaviour identical to the pool's own rule (P3) and leaves no window in
    // which a deferred close keeps the transaction's locks.
    if (api().get_autocommit(state.db) == 0) {
        (void)run_control(state.db, "ROLLBACK");
    }
    state.closed = true;
    if (state.cursors == 0) {
        (void)api().close_v2(state.db);
        state.db = nullptr;
    }
}

bool connection::is_open() const noexcept {
    return impl_ && impl_->state && impl_->state->db != nullptr && !impl_->state->closed;
}

std::string_view connection::journal_mode() const noexcept {
    if (!impl_) {
        return {};
    }
    return impl_->journal_mode;
}

#else // !CRPC_HAVE_SQLITE3

struct connection::impl {};

connection::connection(connection_options options) {
    (void)options;
    connect_error_ = rpc_error{rpc_code::failed_precondition,
                               "this build has no SQLite backend; the sqlite3 library was "
                               "not found"};
}

connection::~connection() = default;

status connection::ping() { return status{connect_error_}; }
bool connection::in_transaction() const noexcept { return false; }
status connection::begin() { return status{connect_error_}; }
status connection::commit() { return status{connect_error_}; }
status connection::rollback() { return status{connect_error_}; }
status connection::set_command_timeout(std::chrono::milliseconds timeout) noexcept {
    (void)timeout;
    return status{connect_error_};
}
status connection::query(std::string_view sql, span<const parameter> params,
                         result_set** out) {
    (void)sql;
    (void)params;
    if (out != nullptr) {
        *out = nullptr;
    }
    return status{connect_error_};
}
status connection::query_streaming(std::string_view sql, span<const parameter> params,
                                   result_set** out) {
    return query(sql, params, out);
}
void connection::close() noexcept {}
bool connection::is_open() const noexcept { return false; }
std::string_view connection::journal_mode() const noexcept { return {}; }

#endif // CRPC_HAVE_SQLITE3

// --- factory -----------------------------------------------------------------

connection_factory::connection_factory(connection_options options)
    : options_(std::move(options)) {}

connection_factory::~connection_factory() = default;

result<db::connection*> connection_factory::open() {
    std::unique_ptr<connection> session(new connection(options_));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::failed_precondition, "the SQLite connection failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "sqlite"; }

} // namespace sqlite
} // namespace db
} // namespace crpc
