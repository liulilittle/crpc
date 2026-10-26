// CRPC -- Redis RTTI binding.
//
// This header bridges the Redis data-type layer (`types.hpp`) and the CRPC
// reflection dependency (`rtti/reflection.hpp`). It turns a `redis::value` into
// a C++ scalar / container / reflected DTO and back, and builds the Redis hash
// commands that store and load a reflected DTO.
//
// Design
// ------
// The conversion is field-by-field, driven by `rtti::fields_of<T>` and
// `rtti::base_types<T>` (the same tables `crpc::detail::flat_members` uses in
// codec.hpp). A reflected struct becomes a Redis hash: each field is one hash
// field whose name is the reflected field name and whose value is the field's
// encoded `redis::value`. A derived struct's base fields come first, in the
// order the reflection tables report them.
//
// Every failure is a value: the decoders return `crpc::status` and the
// encoders/command builders return `crpc::result<...>`. Nothing here throws
// across the ABI; the only `static_assert`s are for types that cannot be
// represented at all (for example `std::string_view` on decode, which would
// dangle).
//
// Field policy (unknown / missing)
// --------------------------------
// A hash reply can carry fields the DTO does not declare, and it can omit
// fields the DTO declares. `bind_options` makes both policies explicit:
//
//   * `on_unknown` -- a reply field that matches no DTO field. Default
//     `reject`: the decode fails with `rpc_code::decode_error`. `ignore`
//     skips it, which is the forward-compatible choice.
//   * `on_missing` -- a DTO field that has no reply entry. Default `keep`:
//     the member keeps whatever value it already held. `reject` fails the
//     decode.
//
// The defaults are strict about schema drift on the reply side (unknown fields
// are a sign the writer and reader disagree) and forgiving on the writer side
// (a DTO field that was never written keeps its default).
//
// Scalar mapping
// --------------
//   C++                 -> redis::value            <- redis::value
//   bool                -> boolean                 <- boolean / integer / "1"/"0"
//   integral            -> integer (unsigned > i64  <- integer / numeric string
//                         max falls back to string)
//   enum                -> underlying integer      <- underlying integer
//   float / double      -> real                     <- real / integer / numeric string
//   std::string         -> string                   <- string
//   std::optional<T>    -> null when empty          <- null -> nullopt
//   std::vector<T> /    -> array                    <- array / set / push
//     std::list / deque
//   std::array<T, N>    -> array                    <- array (size checked)
//   std::map<K, V>      -> map                     <- map / flat pair array
//   reflected struct    -> map (hash)               <- map / flat pair array
//
// A map key must be a string or an integral; an integral key is formatted as
// its decimal text. A reflected struct field whose value is a container cannot
// be stored in a Redis hash, so `hash_set_command` rejects it with
// `rpc_code::encode_error`.
//
// This header includes `types.hpp`, whose stream-entry projection is
// `crpc::redis::redis_stream_entry`. `stream.hpp` declares a different
// `crpc::redis::stream_entry` (it carries a parsed ID) in the same namespace.
// The two spellings are distinct, so this header and `stream.hpp` can be
// included by one translation unit together. This header never includes
// `stream.hpp`.
#ifndef CRPC_REDIS_RTTI_HPP
#define CRPC_REDIS_RTTI_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/redis/types.hpp>

// RTTI availability, mirroring codec.hpp: a public compile definition when the
// build sets it, otherwise probed through `__has_include`.
#if !defined(CRPC_HAVE_RTTI)
#if defined(__has_include)
#if __has_include(<rtti/reflection.hpp>)
#define CRPC_HAVE_RTTI 1
#else
#define CRPC_HAVE_RTTI 0
#endif
#else
#define CRPC_HAVE_RTTI 0
#endif
#endif

#if CRPC_HAVE_RTTI
#include <rtti/reflection.hpp>

namespace crpc {
namespace redis {

// --- Field policy ------------------------------------------------------------

// How a DTO/hash decode treats a reply field the DTO does not declare and a
// DTO field the reply does not carry. See the header comment for the defaults.
struct bind_options {
    enum class unknown_field { reject, ignore };
    enum class missing_field { reject, keep };

    unknown_field on_unknown = unknown_field::reject;
    missing_field on_missing = missing_field::keep;
};

// --- Public conversion API ---------------------------------------------------

// Encodes `in` as a `redis::value`. Returns `rpc_code::encode_error` when the
// type is supported but the conversion is not representable; an unsupported
// type is a compile-time `static_assert`.
template <typename T>
[[nodiscard]] result<value> encode_to_value(const T& in);

// Decodes a `redis::value` into `out`. Returns `rpc_code::decode_error` on a
// shape mismatch, a range mismatch or a malformed numeric string. `out` is
// left untouched on failure.
template <typename T>
[[nodiscard]] status decode_from_value(const value& in, T& out);

// Convenience overload: returns the decoded value or the error. The result is
// value-initialized before decoding, so a failed decode still yields a
// well-defined object when the caller ignores the error.
template <typename T>
[[nodiscard]] result<T> decode_from_value(const value& in);

// --- DTO <-> Redis hash ------------------------------------------------------

// Builds `HSET key field value ...` from a reflected DTO. Every field must
// encode to a scalar (or an empty optional, stored as an empty string); a
// container-valued field is rejected with `rpc_code::encode_error`.
template <typename T>
[[nodiscard]] result<command> hash_set_command(std::string key, const T& dto);

// Builds `HMGET key field ...`, one reply slot per DTO field in declaration
// order. Missing hash fields come back as null replies.
template <typename T>
[[nodiscard]] command hash_mget_command(std::string key);

// Builds `HGETALL key`, the map reply `decode_hash_reply` reads.
[[nodiscard]] inline command hash_get_command(std::string key) {
    return command{"HGETALL", std::move(key)};
}

// Decodes an `HGETALL` reply (a map, or the flat field/value array RESP2
// returns) into a reflected DTO. Applies `opts` for unknown/missing fields.
template <typename T>
[[nodiscard]] status decode_hash_reply(const value& reply, T& out,
                                       const bind_options& opts = bind_options{});

// Decodes an `HMGET` reply (an array of values in DTO field order, null for a
// missing field) into a reflected DTO.
template <typename T>
[[nodiscard]] status decode_hash_values(const value& reply, T& out,
                                        const bind_options& opts = bind_options{});

// --- RESP reply bridge -------------------------------------------------------

// Converts a `resp_value` reply into a `redis::value`. This is the seam that
// lets a `client::execute` reply flow straight into `decode_from_value` /
// `decode_hash_reply`; it never fails because every RESP shape maps onto a
// `value` alternative.
[[nodiscard]] CRPC_API status value_from_resp(const resp_value& reply,
                                              value& out);

namespace rtti_detail {

constexpr std::size_t default_depth_limit = 32;

template <typename...>
struct dependent_false : std::false_type {};

// --- flattened member table --------------------------------------------------
// A derived type's fields are the base fields followed by its own, matching
// the order `rtti::fields_of<T>::desc` reports. The base chain is acyclic by
// language rule, so the recursion terminates.

template <typename Derived, typename Member>
struct as_derived_member;
template <typename Derived, typename Owner, typename Member>
struct as_derived_member<Derived, Member Owner::*> {
    using type = Member Derived::*;
};

template <typename Derived, typename Tuple, std::size_t... I>
auto convert_members_impl(const Tuple& tuple, std::index_sequence<I...>) {
    return std::tuple<typename as_derived_member<
        Derived, std::tuple_element_t<I, Tuple>>::type...>{
        static_cast<typename as_derived_member<
            Derived, std::tuple_element_t<I, Tuple>>::type>(std::get<I>(tuple))...};
}

template <typename Derived, typename Tuple>
auto convert_members(const Tuple& tuple) {
    return convert_members_impl<Derived>(
        tuple, std::make_index_sequence<std::tuple_size<Tuple>::value>{});
}

template <typename Derived>
const auto& flat_members();

template <typename Derived, typename Bases, std::size_t... I>
auto collect_base_members(std::index_sequence<I...>) {
    return std::tuple_cat(convert_members<Derived>(
        flat_members<std::tuple_element_t<I, Bases>>())...);
}

template <typename Derived>
const auto& flat_members() {
    using bases_t = typename rtti::base_types<Derived>::list;
    static const auto members = std::tuple_cat(
        collect_base_members<Derived, bases_t>(
            std::make_index_sequence<std::tuple_size<bases_t>::value>{}),
        rtti::fields_of<Derived>::members);
    return members;
}

template <typename T, std::size_t I>
const char* field_name_at() noexcept {
    return rtti::fields_of<T>::desc[I].name;
}

// --- shape helpers -----------------------------------------------------------

// The sequence alternatives a reply can carry: an array, a set or a push.
inline const array_value* array_like(const value& v) noexcept {
    if (v.is_array()) {
        return &v.as_array();
    }
    if (v.is_set()) {
        return &v.as_set().items;
    }
    if (v.is_push()) {
        return &v.as_push().items;
    }
    return nullptr;
}

// Turns a reply into an ordered list of (field, value) entries. A map maps
// directly; a flat array must have an even element count and pairs up
// consecutive elements (the RESP2 HGETALL shape).
inline bool as_entries(const value& v, std::vector<std::pair<value, value>>& out) {
    if (v.is_map()) {
        out.assign(v.as_map().begin(), v.as_map().end());
        return true;
    }
    const array_value* items = array_like(v);
    if (items == nullptr || items->size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(items->size() / 2);
    for (std::size_t i = 0; i < items->size(); i += 2) {
        out.emplace_back((*items)[i], (*items)[i + 1]);
    }
    return true;
}

inline rpc_error decode_failure(std::string message) {
    return rpc_error{rpc_code::decode_error, std::move(message)};
}

inline rpc_error encode_failure(std::string message) {
    return rpc_error{rpc_code::encode_error, std::move(message)};
}

// --- encode ------------------------------------------------------------------

template <typename T>
result<value> encode_impl(const T& in, std::size_t depth);

template <typename T, std::size_t... I>
result<value> encode_struct_impl(const T& obj, std::size_t depth,
                                 std::index_sequence<I...>) {
    map_value entries;
    entries.reserve(sizeof...(I));
    bool ok = true;
    rpc_error failure{rpc_code::ok};
    const auto step = [&](auto index_constant) {
        constexpr std::size_t index = decltype(index_constant)::value;
        if (!ok) {
            return;
        }
        result<value> field =
            encode_impl(obj.*std::get<index>(flat_members<T>()), depth + 1);
        if (!field.ok()) {
            ok = false;
            failure = field.error();
            return;
        }
        entries.emplace_back(value{std::string(field_name_at<T, index>())},
                             std::move(field).value());
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    if (!ok) {
        return failure;
    }
    return value{std::move(entries)};
}

// True when a map key type can be rendered as Redis hash field text.
template <typename K>
struct is_text_key
    : std::bool_constant<rtti::is_char_string<std::remove_cv_t<K>>::value ||
                         std::is_integral_v<std::remove_cv_t<K>> ||
                         std::is_enum_v<std::remove_cv_t<K>>> {};

template <typename K>
std::string key_text(const K& key) {
    using key_type = std::remove_cv_t<K>;
    if constexpr (rtti::is_char_string<key_type>::value) {
        return std::string(key);
    } else if constexpr (std::is_enum_v<key_type>) {
        using underlying = std::underlying_type_t<key_type>;
        return std::to_string(static_cast<std::uintmax_t>(
            static_cast<underlying>(key)));
    } else {
        if constexpr (std::is_signed_v<key_type>) {
            return std::to_string(static_cast<std::intmax_t>(key));
        } else {
            return std::to_string(static_cast<std::uintmax_t>(key));
        }
    }
}

template <typename T>
result<value> encode_impl(const T& in, std::size_t depth) {
    using type = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<type, bool>) {
        return value{in};
    } else if constexpr (std::is_enum_v<type>) {
        using underlying = std::underlying_type_t<type>;
        return encode_impl(static_cast<underlying>(in), depth);
    } else if constexpr (std::is_integral_v<type>) {
        if constexpr (std::is_signed_v<type>) {
            return value{static_cast<i64>(in)};
        } else {
            if (in <=
                static_cast<type>(std::numeric_limits<i64>::max())) {
                return value{static_cast<i64>(in)};
            }
            // An unsigned value past i64 has no integer spelling; store the
            // decimal text so a round-trip is exact.
            return value{std::to_string(in)};
        }
    } else if constexpr (std::is_floating_point_v<type>) {
        if constexpr (std::is_same_v<type, float> ||
                      std::is_same_v<type, double>) {
            return value{static_cast<double>(in)};
        } else {
            static_assert(dependent_false<type>::value,
                          "crpc-redis-rtti: long double is not supported");
        }
    } else if constexpr (rtti::is_char_string<type>::value) {
        return value{std::string(in)};
    } else if constexpr (rtti::is_optional<type>::value) {
        if (!in.has_value()) {
            return value{nullptr};
        }
        return encode_impl(*in, depth);
    } else if constexpr (rtti::is_sequence<type>::value) {
        array_value items;
        items.reserve(in.size());
        for (const auto& element : in) {
            result<value> encoded = encode_impl(element, depth + 1);
            if (!encoded.ok()) {
                return encoded.error();
            }
            items.push_back(std::move(encoded).value());
        }
        return value{std::move(items)};
    } else if constexpr (rtti::is_fixed_sequence<type>::value) {
        array_value items;
        items.reserve(in.size());
        for (const auto& element : in) {
            result<value> encoded = encode_impl(element, depth + 1);
            if (!encoded.ok()) {
                return encoded.error();
            }
            items.push_back(std::move(encoded).value());
        }
        return value{std::move(items)};
    } else if constexpr (rtti::is_map<type>::value) {
        using key_type = typename type::key_type;
        static_assert(is_text_key<key_type>::value,
                      "crpc-redis-rtti: a map key must be a string or an "
                      "integral; other key types have no Redis hash spelling");
        map_value entries;
        entries.reserve(in.size());
        for (const auto& entry : in) {
            result<value> mapped = encode_impl(entry.second, depth + 1);
            if (!mapped.ok()) {
                return mapped.error();
            }
            entries.emplace_back(value{key_text(entry.first)},
                                 std::move(mapped).value());
        }
        return value{std::move(entries)};
    } else if constexpr (rtti::is_reflected<type>::value) {
        if (depth > default_depth_limit) {
            return encode_failure("crpc-redis-rtti: reflection depth exceeded");
        }
        return encode_struct_impl(
            in, depth,
            std::make_index_sequence<rtti::field_count<type>()>{});
    } else {
        static_assert(dependent_false<type>::value,
                      "crpc-redis-rtti: unsupported field type; use a "
                      "supported scalar, container or a reflected type");
    }
}

// --- decode ------------------------------------------------------------------

template <typename T>
status decode_impl(const value& in, T& out, std::size_t depth,
                   const bind_options& opts);

inline bool string_to_bool(const std::string& text, bool& out) noexcept {
    if (text == "1" || text == "true" || text == "TRUE") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE") {
        out = false;
        return true;
    }
    return false;
}

inline bool integer_from_value(const value& in, bool& out) noexcept {
    if (in.is_bool()) {
        out = in.as_bool();
        return true;
    }
    if (in.is_int()) {
        out = in.as_int() != 0;
        return true;
    }
    if (in.is_string()) {
        return string_to_bool(in.as_string(), out);
    }
    return false;
}

// Defined in rtti.cpp.
[[nodiscard]] CRPC_API bool parse_i64_text(const std::string& text, i64& out) noexcept;
[[nodiscard]] CRPC_API bool parse_u64_text(const std::string& text, u64& out) noexcept;
[[nodiscard]] CRPC_API bool parse_double_text(const std::string& text,
                                             double& out) noexcept;
[[nodiscard]] CRPC_API bool scalar_arg(const value& v, std::string& out) noexcept;

inline bool parse_integer_text(const std::string& text, long long& out) noexcept {
    i64 parsed = 0;
    if (!parse_i64_text(text, parsed)) {
        return false;
    }
    out = static_cast<long long>(parsed);
    return true;
}

inline bool parse_integer_text(const std::string& text,
                               unsigned long long& out) noexcept {
    u64 parsed = 0;
    if (!parse_u64_text(text, parsed)) {
        return false;
    }
    out = static_cast<unsigned long long>(parsed);
    return true;
}

template <typename T>
status decode_integer(const value& in, T& out) {
    using type = std::remove_cv_t<T>;
    if (in.is_int()) {
        const i64 raw = in.as_int();
        if constexpr (std::is_signed_v<type>) {
            if (raw < static_cast<i64>(std::numeric_limits<type>::min()) ||
                raw > static_cast<i64>(std::numeric_limits<type>::max())) {
                return decode_failure("crpc-redis-rtti: integer out of range");
            }
        } else {
            if (raw < 0 ||
                static_cast<u64>(raw) >
                    static_cast<u64>(std::numeric_limits<type>::max())) {
                return decode_failure("crpc-redis-rtti: integer out of range");
            }
        }
        out = static_cast<type>(raw);
        return status{};
    }
    if (in.is_string()) {
        if constexpr (std::is_signed_v<type>) {
            long long parsed = 0;
            if (!parse_integer_text(in.as_string(), parsed) ||
                parsed < static_cast<long long>(
                             std::numeric_limits<type>::min()) ||
                parsed > static_cast<long long>(
                             std::numeric_limits<type>::max())) {
                return decode_failure("crpc-redis-rtti: integer out of range");
            }
            out = static_cast<type>(parsed);
        } else {
            unsigned long long parsed = 0;
            if (!parse_integer_text(in.as_string(), parsed) ||
                parsed > static_cast<unsigned long long>(
                             std::numeric_limits<type>::max())) {
                return decode_failure("crpc-redis-rtti: integer out of range");
            }
            out = static_cast<type>(parsed);
        }
        return status{};
    }
    return decode_failure("crpc-redis-rtti: expected an integer");
}

template <typename T>
status decode_floating(const value& in, T& out) {
    if (in.is_real()) {
        out = static_cast<T>(in.as_real());
        return status{};
    }
    if (in.is_int()) {
        out = static_cast<T>(in.as_int());
        return status{};
    }
    if (in.is_string()) {
        double parsed = 0.0;
        if (!parse_double_text(in.as_string(), parsed)) {
            return decode_failure("crpc-redis-rtti: expected a number");
        }
        out = static_cast<T>(parsed);
        return status{};
    }
    return decode_failure("crpc-redis-rtti: expected a number");
}

template <typename T, std::size_t... I>
status decode_struct_from_entries(T& obj,
                                  const std::vector<std::pair<value, value>>& entries,
                                  std::size_t depth, const bind_options& opts,
                                  std::index_sequence<I...>) {
    std::vector<bool> seen(sizeof...(I), false);
    status result{};
    for (const auto& entry : entries) {
        if (!entry.first.is_string()) {
            return decode_failure("crpc-redis-rtti: hash field name must be a string");
        }
        const std::string& name = entry.first.as_string();
        bool matched = false;
        const auto try_match = [&](auto index_constant) {
            constexpr std::size_t index = decltype(index_constant)::value;
            if (matched || !result.ok()) {
                return;
            }
            if (name == field_name_at<T, index>()) {
                matched = true;
                seen[index] = true;
                result = decode_impl(
                    entry.second, obj.*std::get<index>(flat_members<T>()),
                    depth + 1, opts);
            }
        };
        (try_match(std::integral_constant<std::size_t, I>{}), ...);
        if (!matched && opts.on_unknown == bind_options::unknown_field::reject) {
            return decode_failure("crpc-redis-rtti: unknown hash field '" + name +
                                  "'");
        }
        if (!result.ok()) {
            return result;
        }
    }
    if (opts.on_missing == bind_options::missing_field::reject) {
        for (bool present : seen) {
            if (!present) {
                return decode_failure("crpc-redis-rtti: missing hash field");
            }
        }
    }
    return status{};
}

template <typename T>
status decode_impl(const value& in, T& out, std::size_t depth,
                   const bind_options& opts) {
    using type = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<type, bool>) {
        bool decoded = false;
        if (!integer_from_value(in, decoded)) {
            return decode_failure("crpc-redis-rtti: expected a boolean");
        }
        out = decoded;
        return status{};
    } else if constexpr (std::is_enum_v<type>) {
        using underlying = std::underlying_type_t<type>;
        underlying raw{};
        const status decoded = decode_integer(in, raw);
        if (!decoded.ok()) {
            return decoded;
        }
        out = static_cast<type>(raw);
        return status{};
    } else if constexpr (std::is_integral_v<type>) {
        return decode_integer(in, out);
    } else if constexpr (std::is_floating_point<type>::value) {
        if constexpr (std::is_same_v<type, float> ||
                      std::is_same_v<type, double>) {
            return decode_floating(in, out);
        } else {
            static_assert(dependent_false<type>::value,
                          "crpc-redis-rtti: long double is not supported");
        }
    } else if constexpr (rtti::is_char_string<type>::value) {
        if (!in.is_string()) {
            return decode_failure("crpc-redis-rtti: expected a string");
        }
        out.assign(in.as_string().data(), in.as_string().size());
        return status{};
    } else if constexpr (rtti::is_optional<type>::value) {
        if (in.is_null()) {
            out.reset();
            return status{};
        }
        if (!out.has_value()) {
            out.emplace();
        }
        return decode_impl(in, *out, depth + 1, opts);
    } else if constexpr (rtti::is_sequence<type>::value) {
        const array_value* items = array_like(in);
        if (items == nullptr) {
            return decode_failure("crpc-redis-rtti: expected an array");
        }
        using element_t = typename type::value_type;
        out.clear();
        for (const value& element : *items) {
            element_t decoded{};
            const status result = decode_impl(element, decoded, depth + 1, opts);
            if (!result.ok()) {
                return result;
            }
            out.push_back(std::move(decoded));
        }
        return status{};
    } else if constexpr (rtti::is_fixed_sequence<type>::value) {
        const array_value* items = array_like(in);
        if (items == nullptr) {
            return decode_failure("crpc-redis-rtti: expected an array");
        }
        if (items->size() != out.size()) {
            return decode_failure("crpc-redis-rtti: array size mismatch");
        }
        for (std::size_t i = 0; i < items->size(); ++i) {
            const status result = decode_impl((*items)[i], out[i], depth + 1, opts);
            if (!result.ok()) {
                return result;
            }
        }
        return status{};
    } else if constexpr (rtti::is_map<type>::value) {
        std::vector<std::pair<value, value>> entries;
        if (!as_entries(in, entries)) {
            return decode_failure("crpc-redis-rtti: expected a map");
        }
        using key_type = typename type::key_type;
        using mapped_type = typename type::mapped_type;
        out.clear();
        for (auto& entry : entries) {
            key_type key{};
            status result = decode_impl(entry.first, key, depth + 1, opts);
            if (!result.ok()) {
                return result;
            }
            mapped_type mapped{};
            result = decode_impl(entry.second, mapped, depth + 1, opts);
            if (!result.ok()) {
                return result;
            }
            out.emplace(std::move(key), std::move(mapped));
        }
        return status{};
    } else if constexpr (rtti::is_reflected<type>::value) {
        if (depth > default_depth_limit) {
            return decode_failure("crpc-redis-rtti: reflection depth exceeded");
        }
        std::vector<std::pair<value, value>> entries;
        if (!as_entries(in, entries)) {
            return decode_failure("crpc-redis-rtti: expected a map or a flat "
                                  "field/value array");
        }
        return decode_struct_from_entries(
            out, entries, depth, opts,
            std::make_index_sequence<rtti::field_count<type>()>{});
    } else {
        static_assert(dependent_false<type>::value,
                      "crpc-redis-rtti: unsupported field type; use a "
                      "supported scalar, container or a reflected type");
    }
}

// --- hash reply decoders -----------------------------------------------------

template <typename T, std::size_t... I>
status decode_values_impl(const array_value& items, T& out,
                          const bind_options& opts,
                          std::index_sequence<I...>) {
    if (items.size() != sizeof...(I)) {
        return decode_failure("crpc-redis-rtti: HMGET reply size mismatch");
    }
    status result{};
    const auto step = [&](auto index_constant) {
        constexpr std::size_t index = decltype(index_constant)::value;
        if (!result.ok()) {
            return;
        }
        const value& element = items[index];
        if (element.is_null()) {
            if (opts.on_missing == bind_options::missing_field::reject) {
                result = decode_failure("crpc-redis-rtti: missing hash field");
            }
            return;
        }
        result = decode_impl(element,
                             out.*std::get<index>(flat_members<T>()), 1, opts);
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    return result;
}

} // namespace rtti_detail

// --- public definitions ------------------------------------------------------

template <typename T>
result<value> encode_to_value(const T& in) {
    return rtti_detail::encode_impl(in, 0);
}

template <typename T>
status decode_from_value(const value& in, T& out) {
    return rtti_detail::decode_impl(in, out, 0, bind_options{});
}

template <typename T>
result<T> decode_from_value(const value& in) {
    T out{};
    const status decoded = rtti_detail::decode_impl(in, out, 0, bind_options{});
    if (!decoded.ok()) {
        return decoded.error();
    }
    return out;
}

template <typename T>
result<command> hash_set_command(std::string key, const T& dto) {
    result<value> encoded = rtti_detail::encode_impl(dto, 0);
    if (!encoded.ok()) {
        return encoded.error();
    }
    if (!encoded.value().is_map()) {
        return rtti_detail::encode_failure(
            "crpc-redis-rtti: a reflected DTO must encode to a hash map");
    }
    const map_value& entries = encoded.value().as_map();
    command cmd;
    cmd.reserve(2 + entries.size() * 2);
    cmd.push_back("HSET");
    cmd.push_back(std::move(key));
    for (const auto& entry : entries) {
        if (!entry.first.is_string()) {
            return rtti_detail::encode_failure(
                "crpc-redis-rtti: hash field name must be a string");
        }
        cmd.push_back(entry.first.as_string());
        std::string argument;
        if (!rtti_detail::scalar_arg(entry.second, argument)) {
            return rtti_detail::encode_failure(
                "crpc-redis-rtti: a DTO field that is a container cannot be "
                "stored in a Redis hash; flatten it to a scalar first");
        }
        cmd.push_back(std::move(argument));
    }
    return cmd;
}

template <typename T, std::size_t... I>
command hash_mget_command_impl(std::string key, std::index_sequence<I...>) {
    command cmd;
    cmd.reserve(2 + sizeof...(I));
    cmd.push_back("HMGET");
    cmd.push_back(std::move(key));
    (cmd.push_back(rtti_detail::field_name_at<T, I>()), ...);
    return cmd;
}

template <typename T>
command hash_mget_command(std::string key) {
    return hash_mget_command_impl<T>(
        std::move(key), std::make_index_sequence<rtti::field_count<T>()>{});
}

template <typename T>
status decode_hash_reply(const value& reply, T& out, const bind_options& opts) {
    return rtti_detail::decode_impl(reply, out, 0, opts);
}

template <typename T>
status decode_hash_values(const value& reply, T& out, const bind_options& opts) {
    const array_value* items = rtti_detail::array_like(reply);
    if (items == nullptr) {
        return rtti_detail::decode_failure(
            "crpc-redis-rtti: expected an HMGET array reply");
    }
    return rtti_detail::decode_values_impl(
        *items, out, opts,
        std::make_index_sequence<rtti::field_count<T>()>{});
}

} // namespace redis
} // namespace crpc

#endif // CRPC_HAVE_RTTI
#endif // CRPC_REDIS_RTTI_HPP
