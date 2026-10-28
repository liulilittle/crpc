// CRPC -- database value and parameter types.
//
// These are caller-side value types, never part of an exported signature: they
// hold `std::string` and `std::vector`, whose layout belongs to the CRT (and
// differs between Debug and Release under _ITERATOR_DEBUG_LEVEL). The exported
// interface in `connection.hpp` therefore uses `result<T>` of scalars and
// `std::string_view`, and a caller that wants a `db_value` builds one from the
// cursor accessors.
//
// The type set is deliberately small. A database value model that tries to be
// every backend's native type at once becomes a union of every backend's quirks;
// the honest approach is a small set with an explicit lossy boundary:
//
//   * integers: `int64` and `uint64`, because `BIGINT UNSIGNED` overflows a
//     signed 64-bit value and a cast would silently wrap;
//   * floating point: `double` only. A `float` column widens exactly, and a
//     `DECIMAL` does not: it arrives as `text` so the caller keeps every digit
//     instead of losing precision in a binary float;
//   * text and blob, kept apart because a blob is not a string and re-encoding a
//     binary value through a text path corrupts it;
//   * null, distinct from empty text and from an empty blob.
//
// Time, decimal, UUID and JSON are carried as `text` with the type tag reported
// by `column_info`. That is lossless (the server's own text form is exact) and
// it avoids inventing a temporal model that would differ from every backend's.
#ifndef CRPC_DB_TYPES_HPP
#define CRPC_DB_TYPES_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

// Transaction isolation levels, the four ANSI/ADO.NET levels plus SNAPSHOT,
// which SQL Server supports natively. `unspecified` is the default: the backend's
// own default applies, which is what plain `begin()` already does.
enum class isolation_level : std::uint8_t {
    unspecified,
    read_uncommitted,
    read_committed,
    repeatable_read,
    serializable,
    snapshot
};

// The ANSI name of an isolation level, for the SQL a backend emits. Never returns
// nullptr. `unspecified` is "UNSPECIFIED" so that no nullptr can ever escape.
[[nodiscard]] constexpr const char* to_string(isolation_level level) noexcept {
    switch (level) {
    case isolation_level::read_uncommitted:
        return "READ UNCOMMITTED";
    case isolation_level::read_committed:
        return "READ COMMITTED";
    case isolation_level::repeatable_read:
        return "REPEATABLE READ";
    case isolation_level::serializable:
        return "SERIALIZABLE";
    case isolation_level::snapshot:
        return "SNAPSHOT";
    default:
        return "UNSPECIFIED";
    }
}

// The value kind a column reports. `unknown` means the backend did not say, which
// is common for an expression column; the caller must then decide from the data.
enum class db_type : std::uint8_t {
    unknown,
    boolean,
    int64,
    uint64,
    real,
    text,
    blob,
    decimal,
    date,
    time,
    timestamp,
    uuid,
    json,
    xml
};

// Human-readable name. Never returns nullptr.
[[nodiscard]] CRPC_API const char* to_string(db_type type) noexcept;

// True when the type is carried in the `text` channel. A caller that switches on
// `db_type` needs this to know where the payload is.
[[nodiscard]] constexpr bool is_textual(db_type type) noexcept {
    switch (type) {
    case db_type::text:
    case db_type::decimal:
    case db_type::date:
    case db_type::time:
    case db_type::timestamp:
    case db_type::uuid:
    case db_type::json:
    case db_type::xml:
        return true;
    default:
        return false;
    }
}

// One column's description.
struct column_info {
    std::string name;
    // The type the backend reported, which may be `unknown`.
    db_type type = db_type::unknown;
    // Declared precision and scale for numeric and decimal columns; 0 when the
    // backend does not report them.
    std::uint32_t precision = 0;
    std::uint32_t scale = 0;
    // True when the column may be null. A driver that cannot tell reports false
    // and the caller must still check `is_null`.
    bool nullable = true;

    // ORM mapping metadata. A struct field is matched to a column, so the caller
    // needs to know where the column came from and what it is allowed to be.
    //
    // `original_name` is the column name before any alias (`SELECT id AS user_id`);
    // mapping on `name` alone would bind the alias and silently lose the field when
    // the query changes.
    std::string schema;
    std::string table;
    std::string original_name;
    // Provider column flags. The concrete meaning is the backend's, exposed rather
    // than reinterpreted: an ORM that needs "auto increment" or "unsigned" asks the
    // adapter, and a generic layer that guessed would be wrong for at least one
    // backend.
    std::uint16_t flags = 0;
};

// A value detached from its cursor. Copyable and owning, so it outlives the
// statement that produced it.
class db_value {
public:
    db_value() noexcept = default;

    static db_value make_null() noexcept { return db_value{}; }

    static db_value from_bool(bool value) noexcept {
        db_value out;
        out.type_ = db_type::boolean;
        out.is_null_ = false;
        out.int_ = value ? 1 : 0;
        return out;
    }

    static db_value from_int64(std::int64_t value) noexcept {
        db_value out;
        out.type_ = db_type::int64;
        out.is_null_ = false;
        out.int_ = value;
        return out;
    }

    static db_value from_uint64(std::uint64_t value) noexcept {
        db_value out;
        out.type_ = db_type::uint64;
        out.is_null_ = false;
        out.uint_ = value;
        return out;
    }

    static db_value from_double(double value) noexcept {
        db_value out;
        out.type_ = db_type::real;
        out.is_null_ = false;
        out.real_ = value;
        return out;
    }

    static db_value from_text(std::string value, db_type type = db_type::text) {
        db_value out;
        out.type_ = type;
        out.is_null_ = false;
        out.text_ = std::move(value);
        return out;
    }

    static db_value from_blob(std::vector<std::byte> value) {
        db_value out;
        out.type_ = db_type::blob;
        out.is_null_ = false;
        out.blob_ = std::move(value);
        return out;
    }

    [[nodiscard]] bool is_null() const noexcept { return is_null_; }
    [[nodiscard]] db_type type() const noexcept { return type_; }

    // A wrong accessor is a programming error, not a data condition, so these
    // return a default and the caller checks `type()` first; the alternatives
    // (exceptions or a result per accessor) make every read site noisy for a bug
    // that a single type check removes.
    [[nodiscard]] std::int64_t as_int64() const noexcept { return int_; }
    [[nodiscard]] std::uint64_t as_uint64() const noexcept { return uint_; }
    [[nodiscard]] double as_double() const noexcept { return real_; }
    [[nodiscard]] bool as_bool() const noexcept { return int_ != 0; }
    [[nodiscard]] std::string_view as_text() const noexcept { return text_; }
    [[nodiscard]] const std::vector<std::byte>& as_blob() const noexcept { return blob_; }

private:
    // A null is a separate flag from `type_` so that a null value still reports
    // the column's declared type, which is what a caller needs to decide how to
    // render it.
    db_type type_ = db_type::unknown;
    bool is_null_ = true;
    std::int64_t int_ = 0;
    std::uint64_t uint_ = 0;
    double real_ = 0.0;
    std::string text_;
    std::vector<std::byte> blob_;
};

// Which way a parameter flows. ADO-style commands expose all four because a stored
// procedure can write back through OUT and INOUT parameters, and some providers
// surface a RETURN_VALUE slot.
enum class parameter_direction : std::uint8_t {
    input,
    output,
    input_output,
    return_value
};

// A bound parameter. Kept as a separate type from `db_value` because binding is
// directional: the caller supplies it, and a null parameter is written as SQL
// NULL rather than as an empty value.
struct parameter {
    db_type type = db_type::unknown;
    // For an input or input-output parameter, "the caller supplied NULL". For an
    // output or return-value parameter this is simply true until the server fills
    // it in, so it must not be sent as data.
    bool is_null = true;
    std::int64_t int_value = 0;
    std::uint64_t uint_value = 0;
    double real_value = 0.0;
    std::string text;
    std::vector<std::byte> blob;

    // Positional parameters may leave this empty; a named provider may require it.
    // Stored procedures are usually addressed by name, so the name is carried even
    // when the wire protocol is positional.
    std::string name;
    parameter_direction direction = parameter_direction::input;

    [[nodiscard]] bool is_output() const noexcept {
        return direction == parameter_direction::output ||
               direction == parameter_direction::input_output ||
               direction == parameter_direction::return_value;
    }
    [[nodiscard]] bool sends_value() const noexcept {
        return direction == parameter_direction::input ||
               direction == parameter_direction::input_output;
    }

    static parameter null() { return parameter{}; }

    // An output slot: it carries a type (so the caller knows what to read back) and
    // no value.
    static parameter out(std::string name, db_type type, parameter_direction direction =
                                                             parameter_direction::output) {
        parameter result;
        result.name = std::move(name);
        result.type = type;
        result.direction = direction;
        result.is_null = true;
        return result;
    }

    static parameter from_int64(std::int64_t value) {
        parameter out;
        out.type = db_type::int64;
        out.is_null = false;
        out.int_value = value;
        return out;
    }

    static parameter from_uint64(std::uint64_t value) {
        parameter out;
        out.type = db_type::uint64;
        out.is_null = false;
        out.uint_value = value;
        return out;
    }

    static parameter from_double(double value) {
        parameter out;
        out.type = db_type::real;
        out.is_null = false;
        out.real_value = value;
        return out;
    }

    static parameter from_text(std::string value, db_type type = db_type::text) {
        parameter out;
        out.type = type;
        out.is_null = false;
        out.text = std::move(value);
        return out;
    }

    static parameter from_blob(std::vector<std::byte> value) {
        parameter out;
        out.type = db_type::blob;
        out.is_null = false;
        out.blob = std::move(value);
        return out;
    }
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_TYPES_HPP