// CRPC -- C++ field types to and from database values.
//
// The boundaries here are the ones a real ORM has to draw honestly:
//
//   * a `std::optional<U>` field is the only way to represent SQL NULL. Reading
//     NULL into a non-optional field is a decode error, never a silent default,
//     because a default value is indistinguishable from data.
//   * a scoped enum is stored as its underlying integral value. Reading an
//     integer that does not fit the underlying type is an error.
//   * `std::chrono::system_clock::time_point` is stored as lossless ISO-ish
//     text in the server's own text form (the `timestamp` lane is a text lane
//     in this library's type model, so no precision is invented here).
//   * a DECIMAL column stays text (the library already treats decimal as a text
//     channel); a `std::string` field round-trips the exact server text.
//   * any conversion the server text cannot accept is reported as a status, and
//     the accessors below deliberately fail rather than narrow silently.
//
// All of this is templates (the member type is only known at compile time), so
// the two non-template helpers -- the system-time text codec -- live in
// crpc/orm/src so they stay testable on their own.
#ifndef CRPC_ORM_VALUE_HPP
#define CRPC_ORM_VALUE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/orm/fwd.hpp>

namespace crpc {
namespace orm {
namespace values {

// --- Field-type recognition -------------------------------------------------

template <typename M>
struct is_optional_field : std::false_type {};
template <typename U>
struct is_optional_field<std::optional<U>> : std::true_type {};

template <typename M>
struct is_system_time
    : std::bool_constant<std::is_same<M, std::chrono::system_clock::time_point>::value> {};

// The scalar payload families a field may be. Optional is handled by a partial
// specialization below so the recursion only ever descends into the inner type
// of a real optional; a plain `&&` with `typename M::value_type` would
// instantiate the recursion for every type.
template <typename M>
struct is_supported_scalar
    : std::integral_constant<
          bool, std::is_same<M, bool>::value || std::is_integral<M>::value ||
                    std::is_enum<M>::value || std::is_floating_point<M>::value ||
                    std::is_same<M, std::string>::value || is_system_time<M>::value ||
                    std::is_same<M, std::vector<std::byte>>::value> {};

template <typename M>
struct is_supported : is_supported_scalar<M> {};

template <typename U>
struct is_supported<std::optional<U>>
    : std::bool_constant<is_supported<U>::value> {};

// --- Number plumbing ---------------------------------------------------------

template <typename M>
inline status fit_signed(std::int64_t value, M& out) {
    if (value < static_cast<std::int64_t>((std::numeric_limits<M>::min)()) ||
        value > static_cast<std::int64_t>((std::numeric_limits<M>::max)())) {
        return status{rpc_code::decode_error, "integer value does not fit the target field type"};
    }
    out = static_cast<M>(value);
    return status{};
}

template <typename M>
inline status fit_unsigned(std::uint64_t value, M& out) {
    if (value > static_cast<std::uint64_t>((std::numeric_limits<M>::max)())) {
        return status{rpc_code::decode_error, "integer value does not fit the target field type"};
    }
    out = static_cast<M>(value);
    return status{};
}

// --- System time codec (defined in src/orm/value.cpp) ------------------------

// Formats a time_point as `YYYY-MM-DD HH:MM:SS` UTC (second precision).
CRPC_API std::string format_system_time(std::chrono::system_clock::time_point value);

// Parses `YYYY-MM-DD (T) HH:MM:SS[.ffffff][Z|+HH:MM]` into a time_point.
// A malformed string is a decode error, never a zero time_point.
CRPC_API status parse_system_time(std::string_view text,
                                  std::chrono::system_clock::time_point& out);

// --- The C++ to parameter direction ------------------------------------------

template <typename M>
inline db_type type_of_field();

// Encodes one field value into a bound parameter. Conversion cannot fail for
// the supported scalar set; the status is still returned so a caller does not
// have to guess which encode paths can fail.
template <typename M>
inline status encode_value(const M& value, parameter& out) {
    if constexpr (is_optional_field<M>::value) {
        using U = typename M::value_type;
        if (!value.has_value()) {
            out = parameter::null();
            out.type = type_of_field<U>();
            return status{};
        }
        return encode_value(*value, out);
    } else if constexpr (std::is_same<M, bool>::value) {
        out = parameter::from_int64(value ? 1 : 0);
        out.type = db_type::boolean;
        return status{};
    } else if constexpr (std::is_integral<M>::value) {
        if constexpr (std::is_signed<M>::value) {
            out = parameter::from_int64(static_cast<std::int64_t>(value));
        } else {
            out = parameter::from_uint64(static_cast<std::uint64_t>(value));
        }
        return status{};
    } else if constexpr (std::is_enum<M>::value) {
        using B = std::underlying_type_t<M>;
        if constexpr (std::is_signed<B>::value) {
            out = parameter::from_int64(static_cast<std::int64_t>(value));
        } else {
            out = parameter::from_uint64(static_cast<std::uint64_t>(value));
        }
        return status{};
    } else if constexpr (std::is_floating_point<M>::value) {
        out = parameter::from_double(static_cast<double>(value));
        return status{};
    } else if constexpr (std::is_same<M, std::string>::value) {
        out = parameter::from_text(value);
        return status{};
    } else if constexpr (is_system_time<M>::value) {
        out = parameter::from_text(format_system_time(value), db_type::timestamp);
        return status{};
    } else if constexpr (std::is_same<M, std::vector<std::byte>>::value) {
        out = parameter::from_blob(value);
        return status{};
    } else {
        return status{rpc_code::encode_error, "unsupported field type"};
    }
}

// The db_type a field of type M maps to. Mirrors encode_value so an optional
// and its inner type agree on the carrier.
template <typename M>
inline db_type type_of_field() {
    if constexpr (is_optional_field<M>::value) {
        return type_of_field<typename M::value_type>();
    } else if constexpr (std::is_same<M, bool>::value) {
        return db_type::boolean;
    } else if constexpr (std::is_integral<M>::value) {
        return std::is_signed<M>::value ? db_type::int64 : db_type::uint64;
    } else if constexpr (std::is_enum<M>::value) {
        using B = std::underlying_type_t<M>;
        return std::is_signed<B>::value ? db_type::int64 : db_type::uint64;
    } else if constexpr (std::is_floating_point<M>::value) {
        return db_type::real;
    } else if constexpr (std::is_same<M, std::string>::value) {
        return db_type::text;
    } else if constexpr (is_system_time<M>::value) {
        return db_type::timestamp;
    } else if constexpr (std::is_same<M, std::vector<std::byte>>::value) {
        return db_type::blob;
    } else {
        return db_type::unknown;
    }
}

// Encodes a bare scalar (as used for a primary-key lookup). Understands the
// string literal family in addition to the field types.
template <typename V>
inline status encode_scalar(const V& raw, parameter& out) {
    using U = std::decay_t<V>;
    if constexpr (std::is_same<U, const char*>::value || std::is_same<U, char*>::value ||
                  std::is_same<U, std::string_view>::value) {
        out = parameter::from_text(std::string(raw));
        return status{};
    } else if constexpr (std::is_same<U, std::string>::value) {
        out = parameter::from_text(raw);
        return status{};
    } else if constexpr (std::is_same<U, bool>::value) {
        out = parameter::from_int64(raw ? 1 : 0);
        out.type = db_type::boolean;
        return status{};
    } else if constexpr (std::is_integral<U>::value) {
        if constexpr (std::is_signed<U>::value) {
            out = parameter::from_int64(static_cast<std::int64_t>(raw));
        } else {
            out = parameter::from_uint64(static_cast<std::uint64_t>(raw));
        }
        return status{};
    } else if constexpr (std::is_enum<U>::value) {
        using B = std::underlying_type_t<U>;
        if constexpr (std::is_signed<B>::value) {
            out = parameter::from_int64(static_cast<std::int64_t>(raw));
        } else {
            out = parameter::from_uint64(static_cast<std::uint64_t>(raw));
        }
        return status{};
    } else if constexpr (std::is_floating_point<U>::value) {
        out = parameter::from_double(static_cast<double>(raw));
        return status{};
    } else {
        return status{rpc_code::invalid_argument, "unsupported key value type"};
    }
}

// --- The database to C++ direction -------------------------------------------

// Decodes the cell at `col` of the current cursor row into `out`. NULL handling
// is strict: only an optional field can hold NULL.
template <typename M>
inline status decode_cell(result_set& set, std::size_t col, M& out);

namespace detail {

template <typename M>
inline status decode_non_null(result_set& set, std::size_t col, M& out) {
    if constexpr (std::is_same<M, bool>::value) {
        auto value = set.get_bool(col);
        if (!value) {
            return value.error();
        }
        out = value.value();
        return status{};
    } else if constexpr (std::is_integral<M>::value) {
        if constexpr (std::is_signed<M>::value) {
            auto value = set.get_int64(col);
            if (!value) {
                return value.error();
            }
            return fit_signed(value.value(), out);
        } else {
            auto value = set.get_uint64(col);
            if (!value) {
                return value.error();
            }
            return fit_unsigned(value.value(), out);
        }
    } else if constexpr (std::is_enum<M>::value) {
        using B = std::underlying_type_t<M>;
        if constexpr (std::is_signed<B>::value) {
            auto value = set.get_int64(col);
            if (!value) {
                return value.error();
            }
            B underlying = 0;
            const status fit = fit_signed(value.value(), underlying);
            if (!fit) {
                return fit;
            }
            out = static_cast<M>(underlying);
            return status{};
        } else {
            auto value = set.get_uint64(col);
            if (!value) {
                return value.error();
            }
            B underlying = 0;
            const status fit = fit_unsigned(value.value(), underlying);
            if (!fit) {
                return fit;
            }
            out = static_cast<M>(underlying);
            return status{};
        }
    } else if constexpr (std::is_floating_point<M>::value) {
        auto value = set.get_double(col);
        if (!value) {
            return value.error();
        }
        out = static_cast<M>(value.value());
        return status{};
    } else if constexpr (std::is_same<M, std::string>::value) {
        auto value = set.get_text(col);
        if (!value) {
            return value.error();
        }
        out.assign(value.value().data(), value.value().size());
        return status{};
    } else if constexpr (is_system_time<M>::value) {
        auto value = set.get_text(col);
        if (!value) {
            return value.error();
        }
        return parse_system_time(value.value(), out);
    } else if constexpr (std::is_same<M, std::vector<std::byte>>::value) {
        auto value = set.get_blob(col);
        if (!value) {
            return value.error();
        }
        out = std::move(value.value());
        return status{};
    } else {
        return status{rpc_code::decode_error, "unsupported field type"};
    }
}

} // namespace detail

template <typename M>
inline status decode_cell(result_set& set, std::size_t col, M& out) {
    if constexpr (is_optional_field<M>::value) {
        using U = typename M::value_type;
        if (set.is_null(col)) {
            out.reset();
            return status{};
        }
        U inner{};
        const status decoded = detail::decode_non_null(set, col, inner);
        if (!decoded) {
            return decoded;
        }
        out = std::move(inner);
        return status{};
    } else {
        if (set.is_null(col)) {
            return status{rpc_code::decode_error,
                          "NULL column value read into a non-optional field"};
        }
        return detail::decode_non_null(set, col, out);
    }
}

// Decodes a detached value (used to write the generated auto-increment
// identifier back into the entity's primary key field).
template <typename M>
inline status decode_value_into(const db_value& value, M& out) {
    if constexpr (is_optional_field<M>::value) {
        using U = typename M::value_type;
        if (value.is_null()) {
            out.reset();
            return status{};
        }
        U inner{};
        const status decoded = decode_value_into(value, inner);
        if (!decoded) {
            return decoded;
        }
        out = std::move(inner);
        return status{};
    } else {
        if (value.is_null()) {
            return status{rpc_code::decode_error,
                          "NULL value read into a non-optional field"};
        }
        if constexpr (std::is_same<M, bool>::value) {
            if (value.type() != db_type::boolean) {
                return status{rpc_code::decode_error, "value is not a boolean"};
            }
            out = value.as_bool();
            return status{};
        } else if constexpr (std::is_integral<M>::value) {
            const db_type t = value.type();
            if (t != db_type::int64 && t != db_type::uint64 && t != db_type::boolean) {
                return status{rpc_code::decode_error, "value is not an integer"};
            }
            if constexpr (std::is_signed<M>::value) {
                if (t == db_type::uint64) {
                    const std::uint64_t number = value.as_uint64();
                    if (number > static_cast<std::uint64_t>((std::numeric_limits<M>::max)())) {
                        return status{rpc_code::decode_error,
                                      "integer value does not fit the target field type"};
                    }
                    out = static_cast<M>(number);
                    return status{};
                }
                return fit_signed(value.as_int64(), out);
            } else {
                if (t == db_type::uint64) {
                    return fit_unsigned(value.as_uint64(), out);
                }
                const std::int64_t signed_value = value.as_int64();
                if (signed_value < 0) {
                    return status{rpc_code::decode_error,
                                  "negative value read into an unsigned field"};
                }
                return fit_unsigned(static_cast<std::uint64_t>(signed_value), out);
            }
        } else if constexpr (std::is_enum<M>::value) {
            using B = std::underlying_type_t<M>;
            const db_type t = value.type();
            if (t != db_type::int64 && t != db_type::uint64 && t != db_type::boolean) {
                return status{rpc_code::decode_error, "value is not an integer"};
            }
            if constexpr (std::is_signed<B>::value) {
                if (t == db_type::uint64 && value.as_uint64() >
                                              static_cast<std::uint64_t>(
                                                  (std::numeric_limits<B>::max)())) {
                    return status{rpc_code::decode_error,
                                  "integer value does not fit the target field type"};
                }
                B underlying = 0;
                const status fit = fit_signed(
                    t == db_type::uint64 ? static_cast<std::int64_t>(value.as_uint64())
                                         : value.as_int64(),
                    underlying);
                if (!fit) {
                    return fit;
                }
                out = static_cast<M>(underlying);
                return status{};
            } else {
                if (t == db_type::int64 && value.as_int64() < 0) {
                    return status{rpc_code::decode_error,
                                  "negative value read into an unsigned enum field"};
                }
                B underlying = 0;
                const status fit = fit_unsigned(
                    t == db_type::uint64 ? value.as_uint64()
                                         : static_cast<std::uint64_t>(value.as_int64()),
                    underlying);
                if (!fit) {
                    return fit;
                }
                out = static_cast<M>(underlying);
                return status{};
            }
        } else if constexpr (std::is_floating_point<M>::value) {
            const db_type t = value.type();
            if (t != db_type::real && t != db_type::int64 && t != db_type::uint64) {
                return status{rpc_code::decode_error, "value is not a number"};
            }
            if (t == db_type::real) {
                out = static_cast<M>(value.as_double());
            } else if (t == db_type::uint64) {
                out = static_cast<M>(static_cast<double>(value.as_uint64()));
            } else {
                out = static_cast<M>(static_cast<double>(value.as_int64()));
            }
            return status{};
        } else if constexpr (std::is_same<M, std::string>::value) {
            if (value.type() == db_type::blob) {
                return status{rpc_code::decode_error,
                              "blob value read into a text field"};
            }
            out.assign(value.as_text().data(), value.as_text().size());
            return status{};
        } else if constexpr (is_system_time<M>::value) {
            if (value.type() != db_type::timestamp && value.type() != db_type::date &&
                value.type() != db_type::time && value.type() != db_type::text) {
                return status{rpc_code::decode_error, "value is not a timestamp"};
            }
            return parse_system_time(value.as_text(), out);
        } else if constexpr (std::is_same<M, std::vector<std::byte>>::value) {
            if (value.type() != db_type::blob) {
                return status{rpc_code::decode_error, "value is not a blob"};
            }
            out = value.as_blob();
            return status{};
        } else {
            return status{rpc_code::decode_error, "unsupported field type"};
        }
    }
}

// --- Change detection --------------------------------------------------------

template <typename M>
inline bool equal_value(const M& a, const M& b) {
    if constexpr (is_optional_field<M>::value) {
        if (!a.has_value() && !b.has_value()) {
            return true;
        }
        if (!a.has_value() || !b.has_value()) {
            return false;
        }
        return equal_value(*a, *b);
    } else {
        return a == b;
    }
}

} // namespace values
} // namespace orm
} // namespace crpc

#endif // CRPC_ORM_VALUE_HPP