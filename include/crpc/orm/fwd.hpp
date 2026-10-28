// CRPC -- ORM forward declarations shared by every orm header.
#ifndef CRPC_ORM_FWD_HPP
#define CRPC_ORM_FWD_HPP

#include <cstddef>
#include <cstdint>

#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>

namespace crpc {
namespace orm {

// The ORM speaks the library's own database value model; the four names below
// are used unqualified through every orm header, so they are injected here once
// rather than spelled `db::parameter` throughout the templates.
using crpc::db::db_type;
using crpc::db::db_value;
using crpc::db::parameter;
using crpc::db::result_set;
using crpc::db::statement_guard;

// The SQL flavour the generated statements are written in. The abstraction is
// deliberately small: what actually differs between the three supported
// backends here is identifier quoting and the way a generated sequence value is
// read back. Placeholders are `?` everywhere (this library's connection
// interface binds positional parameters for every backend), so the dialect does
// not need to know about placeholder styles.
enum class dialect : std::uint8_t {
    mysql, // `ident`, SELECT LAST_INSERT_ID()
    tsql,  // [ident], SELECT SCOPE_IDENTITY(); no LIMIT support
    ansi   // "ident", portable or generic ODBC
};

// Sentinel for "no such column / not indexed".
inline constexpr std::size_t orm_npos = static_cast<std::size_t>(-1);

// Mirrors `db::db_type` families so a runtime compatibility check can decide
// whether a caller-supplied key/criterion fits a column without pretending to
// understand the full matrix.
enum class type_family : std::uint8_t {
    boolean,
    integral,
    real,
    textual,
    blob,
    other
};

} // namespace orm
} // namespace crpc

#endif // CRPC_ORM_FWD_HPP