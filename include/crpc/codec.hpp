// CRPC -- codec registry and typed serialization interface.
//
// This header defines the type-erased `codec` boundary used by the RPC and HTTP
// layers plus the typed helpers (`encode_into<T>` / `decode_from<T>`) that the
// hot path uses. The typed helpers perform a single `id()` check and then call
// the concrete codec's member template directly: there is no std::function, no
// per-field virtual dispatch and no intermediate document tree.
//
// Formats
// -------
//   * crpc-binary -- the default, complete codec. It walks a flattened
//     member-pointer tuple at compile time with `if constexpr` and fold
//     expressions. For a derived type the tuple is built once at first use by
//     recursively combining `rtti::base_types<T>::list` with
//     `rtti::fields_of<T>::members`, so the base chain is acyclic by language
//     rule and the resulting field ids follow declaration order.
//   * JSON -- the HTTP/debug/cross-language path. It is wired to
//     `rtti::json::serialize<T>` / `rtti::json::parse_as<T>`.
//   * XML, INI, TOML -- registered behind CRPC_HAVE_RTTI_<FORMAT>.
//   * protobuf, FlatBuffers -- registered behind CRPC_HAVE_RTTI_<FORMAT>; both
//     require an explicit user schema specialization. Using one without a
//     schema fails with a clear diagnostic (a compile-time static_assert in the
//     format adapter, a typed error status through encode_into/decode_from);
//     it is never a silent empty payload.
//
// crpc-binary v1 wire format
// --------------------------
// Every field is `varint field_id, varint payload_len, payload`. The explicit
// length is the price paid so that an unknown field id can always be skipped
// without knowing its schema; the alternative (schema-known fields only) would
// make forward compatibility impossible. Field ids are the declaration index
// plus one, base fields first for derived types.
//
//   payload := bool      | 1 byte 0/1
//            | signed    | zigzag varint (i8/i16/i32/i64, signed enum)
//            | unsigned  | varint (u8/u16/u32/u64, unsigned enum)
//            | float     | 4 bytes little-endian IEEE-754
//            | double    | 8 bytes little-endian IEEE-754
//            | text      | UTF-8 bytes (bounded by payload_len)
//            | bytes     | raw bytes (bounded by payload_len)
//            | sequence  | varint count, then elements:
//                          scalar elements are packed with no per-element length;
//                          other elements are `varint len, payload`.
//            | map       | varint count, then per entry
//                          `varint key_len, key, varint value_len, value`.
//            | optional  | 1 byte presence (0/1), then payload when present.
//            | message   | recursive field sequence (bounded by payload_len).
//
// Missing fields leave the existing value untouched unless the type is
// registered with RTTI_DEFINE_TYPE_WITH_DEFAULT, in which case the object is
// value-initialized before decoding. Duplicate ids, id 0, truncated payloads,
// lengths beyond the remaining input, depth over the limit and (in strict
// mode) trailing bytes inside a scalar payload are rejected with
// rpc_code::decode_error. Unknown ids are skipped.
//
// Depth limit: 32 nested reflected/container/optional levels by default.
// Strict mode: on by default; when disabled, trailing bytes in a scalar payload
// are ignored instead of rejected.
//
// The erased `codec::encode` / `codec::decode` entry points need the value type
// to be registered with `register_type<T>()` first. `encode_into<T>` /
// `decode_from<T>` do not need registration because they dispatch directly to
// the compile-time template.
#ifndef CRPC_CODEC_HPP
#define CRPC_CODEC_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/codec_id.hpp>
#include <crpc/config.hpp>
#include <crpc/endian.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/wire.hpp>

// --- Optional RTTI dependency ------------------------------------------------
//
// RTTI is consumed read-only through its public headers. `__has_include` keeps
// this header standalone-compilable when the dependency is absent; in that
// configuration the binary codec is a stub that reports encode/decode errors.
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

// --- typeid availability ------------------------------------------------------
//
// `CRPC_HAVE_RTTI` above means "the rtti reflection headers are available". It
// is orthogonal to whether this translation unit can write `typeid`, which
// `-fno-rtti` (GCC/Clang) and `/GR-` (MSVC) affect. A build can have the
// reflection headers on the include path and still reject `typeid`, so the two
// questions are tested separately.
//
// The macro below is derived from the compiler and must not be defined by the
// caller: it selects which branch of inline function templates is compiled, so
// two translation units that disagree would define the same specialization
// twice -- an ODR violation the linker resolves silently, keeping one branch
// and discarding the other's calls. Defining it is therefore a hard error.
//
// That #error only closes the -D route. Mixing RTTI flags across the translation
// units of one binary (some built with -fno-rtti, some without) still diverges
// the same way, and no header can detect it: the flag must be uniform for the
// whole binary. Measured with two TUs sharing one inline instantiation, the
// winner depended on link order (Clang: reversed order reported the other
// branch's result).
//
// Per compiler:
//
//   * GCC/Clang `-fno-rtti` removes `typeid`; `__cpp_rtti` is the documented
//     feature-test macro and is absent there.
//   * clang-cl defines `__cpp_rtti` even under `/GR-` and still accepts the
//     static form `typeid(T)`, which is the only form this library uses.
//   * MSVC (cl) also accepts the static form `typeid(T)` under `/GR-`. `/GR-`
//     removes the RTTI *data* that the dynamic forms need; the static form is
//     accepted (MSVC shows exactly that shape in its C4541 example) and a
//     `/GR-` build registers and runs the type-erased path with bytes identical
//     to `/GR`. This is verified by probe, not a formal guarantee, so re-run it
//     when the toolchain changes:
//
//         cl /nologo /std:c++17 /GR- /EHsc /c probe.cpp  # probe: &typeid(T)
//
//     Treating `/GR-` as "no typeid" would turn that working configuration into
//     an error, so cl always reports 1.
//
// `/GR-` is only usable when the reflection dependency is absent or warnings
// are not errors: rtti/proto.hpp uses `dynamic_cast`, which `/GR-` turns into
// C4541, and a `/WX` build then fails in that third-party header (C2220). The
// capability this macro reports is the ability to write `typeid`, not a promise
// that every dependency compiles under `/GR-`.
#if defined(CRPC_CAN_USE_TYPEID)
#error "CRPC_CAN_USE_TYPEID is derived from the compiler; do not define it"
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define CRPC_CAN_USE_TYPEID 1
#elif defined(__cpp_rtti)
#define CRPC_CAN_USE_TYPEID 1
#else
#define CRPC_CAN_USE_TYPEID 0
#endif

#if CRPC_HAVE_RTTI
#include <rtti/reflection.hpp>

#if defined(__has_include)
#if __has_include(<rtti/json.hpp>) && \
    (!defined(RTTI_ENABLE_JSON) || RTTI_ENABLE_JSON) && \
    (!defined(CRPC_HAVE_RTTI_JSON) || CRPC_HAVE_RTTI_JSON)
#define CRPC_HAVE_RTTI_JSON 1
#include <rtti/json.hpp>
#endif
#if __has_include(<rtti/xml.hpp>) && \
    (!defined(RTTI_ENABLE_XML) || RTTI_ENABLE_XML) && \
    (!defined(CRPC_HAVE_RTTI_XML) || CRPC_HAVE_RTTI_XML)
#define CRPC_HAVE_RTTI_XML 1
#include <rtti/xml.hpp>
#endif
#if __has_include(<rtti/ini.hpp>) && \
    (!defined(RTTI_ENABLE_INI) || RTTI_ENABLE_INI) && \
    (!defined(CRPC_HAVE_RTTI_INI) || CRPC_HAVE_RTTI_INI)
#define CRPC_HAVE_RTTI_INI 1
#include <rtti/ini.hpp>
#endif
#if __has_include(<rtti/toml.hpp>) && \
    (!defined(RTTI_ENABLE_TOML) || RTTI_ENABLE_TOML) && \
    (!defined(CRPC_HAVE_RTTI_TOML) || CRPC_HAVE_RTTI_TOML)
#define CRPC_HAVE_RTTI_TOML 1
#include <rtti/toml.hpp>
#endif
#if __has_include(<rtti/proto.hpp>) && \
    (!defined(RTTI_ENABLE_PROTO) || RTTI_ENABLE_PROTO) && \
    (!defined(CRPC_HAVE_RTTI_PROTOBUF) || CRPC_HAVE_RTTI_PROTOBUF)
#define CRPC_HAVE_RTTI_PROTOBUF 1
#include <rtti/proto.hpp>
#endif
#if __has_include(<rtti/flat.hpp>) && \
    (!defined(RTTI_ENABLE_FLATBUFFERS) || RTTI_ENABLE_FLATBUFFERS) && \
    (!defined(CRPC_HAVE_RTTI_FLATBUFFERS) || CRPC_HAVE_RTTI_FLATBUFFERS)
#define CRPC_HAVE_RTTI_FLATBUFFERS 1
#include <rtti/flat.hpp>
#endif
#endif
#endif

namespace crpc {

// The codec id type and its numeric values live in <crpc/codec_id.hpp>; the
// registry and the frame codec must agree, so there is exactly one definition.

// Type-erased codec boundary. Implementations must be immutable after
// construction (apart from their internal type-registration cache) so that a
// codec may be read concurrently after the registry is frozen.
class codec {
public:
    virtual ~codec() = default;

    [[nodiscard]] virtual codec_id id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view content_type() const noexcept = 0;
    [[nodiscard]] virtual status encode(const void* value, const std::type_info& type,
                                        buffer& out) const = 0;
    [[nodiscard]] virtual status decode(span<const std::byte> in, void* value,
                                        const std::type_info& type) const = 0;
};

namespace detail {

template <typename...>
struct dependent_false : std::false_type {};

// --- varint / fixed-width writers -------------------------------------------

inline bool put_varint(buffer& out, u64 value) {
    while (value >= 0x80u) {
        if (!out.push_back(static_cast<std::byte>((value & 0x7Fu) | 0x80u))) {
            return false;
        }
        value >>= 7;
    }
    return out.push_back(static_cast<std::byte>(value));
}

inline bool put_zigzag(buffer& out, i64 value) {
    const u64 raw = static_cast<u64>(value);
    return put_varint(out, (raw << 1) ^ (0u - (raw >> 63)));
}

inline bool put_fixed32(buffer& out, u32 value) {
    std::byte bytes[4];
    endian::store_le32(bytes, value);
    return out.append(bytes, sizeof(bytes));
}

inline bool put_fixed64(buffer& out, u64 value) {
    std::byte bytes[8];
    endian::store_le64(bytes, value);
    return out.append(bytes, sizeof(bytes));
}

// --- type classification -----------------------------------------------------

template <typename T>
struct is_byte_sequence : std::false_type {};
template <typename Alloc>
struct is_byte_sequence<std::vector<std::byte, Alloc>> : std::true_type {};
template <typename Alloc>
struct is_byte_sequence<std::vector<std::uint8_t, Alloc>> : std::true_type {};
template <typename Alloc>
struct is_byte_sequence<std::vector<std::int8_t, Alloc>> : std::true_type {};
template <std::size_t N>
struct is_byte_sequence<std::array<std::byte, N>> : std::true_type {};

template <typename T>
struct is_text : std::false_type {};
template <typename Traits, typename Alloc>
struct is_text<std::basic_string<char, Traits, Alloc>> : std::true_type {};
template <typename Traits>
struct is_text<std::basic_string_view<char, Traits>> : std::true_type {};

template <typename T>
inline constexpr bool is_wire_scalar_v =
    std::is_arithmetic_v<T> || std::is_enum_v<T>;

// --- binary encode -----------------------------------------------------------

#if CRPC_HAVE_RTTI
template <typename T>
status encode_value(buffer& out, const T& value, std::size_t depth,
                    std::size_t limit);

// --- flattened field table ---------------------------------------------------
//
// A derived type's fields are the base fields followed by its own. RTTI exposes
// the two parts separately (`base_types<T>::list` and `fields_of<T>::members`),
// and a pointer-to-member of a base converts implicitly to a pointer-to-member
// of the derived type. Flattening them once, at first use, lets the encoder and
// decoder walk a single ordered list and keeps field ids identical to the
// declaration order the specification promises. Doing this at runtime rather
// than in a constant expression is deliberate: std::tuple_cat is not a constant
// expression on every supported compiler.

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
    // Recursive: a base may itself be derived, and `fields_of<B>::members`
    // holds only B's own fields. The base chain is acyclic by language rule,
    // so this recursion terminates without a depth counter.
    return std::tuple_cat(convert_members<Derived>(
        flat_members<std::tuple_element_t<I, Bases>>())...);
}

template <typename Derived>
const auto& flat_members() {
    using bases_t = typename rtti::base_types<Derived>::list;
    static const auto value = std::tuple_cat(
        collect_base_members<Derived, bases_t>(
            std::make_index_sequence<std::tuple_size<bases_t>::value>{}),
        rtti::fields_of<Derived>::members);
    return value;
}

template <typename T>
status encode_field(buffer& out, u64 id, const T& value, std::size_t depth,
                    std::size_t limit) {
    buffer payload;
    const status encoded = encode_value(payload, value, depth + 1, limit);
    if (!encoded.ok()) {
        return encoded;
    }
    if (!put_varint(out, id)) {
        return status{rpc_code::encode_error};
    }
    if (!put_varint(out, static_cast<u64>(payload.size()))) {
        return status{rpc_code::encode_error};
    }
    if (!out.append(payload.readable())) {
        return status{rpc_code::encode_error};
    }
    return status{};
}

template <typename T, typename Tuple, std::size_t... I>
status encode_members(const T& value, const Tuple& members, u64 offset,
                      buffer& out, std::size_t depth, std::size_t limit,
                      std::index_sequence<I...>) {
    status result;
    const auto step = [&](auto index_constant) {
        constexpr std::size_t index = decltype(index_constant)::value;
        if (result.ok()) {
            result = encode_field(out, offset + index + 1,
                                  value.*std::get<index>(members), depth, limit);
        }
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    return result;
}

template <typename T>
status encode_struct_fields(const T& value, buffer& out, u64 id_offset,
                            std::size_t depth, std::size_t limit);


template <typename T>
status encode_struct_fields(const T& value, buffer& out, u64 id_offset,
                            std::size_t depth, std::size_t limit) {
    if (depth > limit) {
        return status{rpc_code::encode_error};
    }
    const auto& members = flat_members<T>();
    using members_t = std::decay_t<decltype(members)>;
    return encode_members(value, members, id_offset, out, depth, limit,
                          std::make_index_sequence<
                              std::tuple_size<members_t>::value>{});
}

template <typename T>
status encode_value(buffer& out, const T& value, std::size_t depth,
                    std::size_t limit) {
    using type = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<type, bool>) {
        return out.push_back(value ? std::byte{1} : std::byte{0})
                   ? status{}
                   : status{rpc_code::encode_error};
    } else if constexpr (std::is_enum_v<type>) {
        using underlying = std::underlying_type_t<type>;
        const underlying raw = static_cast<underlying>(value);
        if constexpr (std::is_signed_v<underlying>) {
            return put_zigzag(out, static_cast<i64>(raw))
                       ? status{}
                       : status{rpc_code::encode_error};
        } else {
            return put_varint(out, static_cast<u64>(raw))
                       ? status{}
                       : status{rpc_code::encode_error};
        }
    } else if constexpr (std::is_integral_v<type>) {
        if constexpr (std::is_signed_v<type>) {
            return put_zigzag(out, static_cast<i64>(value))
                       ? status{}
                       : status{rpc_code::encode_error};
        } else {
            return put_varint(out, static_cast<u64>(value))
                       ? status{}
                       : status{rpc_code::encode_error};
        }
    } else if constexpr (std::is_floating_point_v<type>) {
        if constexpr (std::is_same_v<type, float>) {
            return put_fixed32(out, endian::float_to_bits(value))
                       ? status{}
                       : status{rpc_code::encode_error};
        } else if constexpr (std::is_same_v<type, double>) {
            return put_fixed64(out, endian::double_to_bits(value))
                       ? status{}
                       : status{rpc_code::encode_error};
        } else {
            static_assert(dependent_false<type>::value,
                          "crpc-binary: long double is not supported");
        }
    } else if constexpr (is_text<type>::value) {
        if (value.empty()) {
            return status{};
        }
        return out.append(value.data(), value.size())
                   ? status{}
                   : status{rpc_code::encode_error};
    } else if constexpr (is_byte_sequence<type>::value) {
        if (!value.empty() &&
            !out.append(value.data(), value.size())) {
            return status{rpc_code::encode_error};
        }
        return status{};
    } else if constexpr (rtti::is_optional<type>::value) {
        if (depth > limit) {
            return status{rpc_code::encode_error};
        }
        if (!value.has_value()) {
            return out.push_back(std::byte{0}) ? status{}
                                               : status{rpc_code::encode_error};
        }
        if (!out.push_back(std::byte{1})) {
            return status{rpc_code::encode_error};
        }
        return encode_value(out, *value, depth + 1, limit);
    } else if constexpr (rtti::is_map<type>::value) {
        if (depth > limit) {
            return status{rpc_code::encode_error};
        }
        if (!put_varint(out, static_cast<u64>(value.size()))) {
            return status{rpc_code::encode_error};
        }
        for (const auto& entry : value) {
            buffer key;
            const status key_status =
                encode_value(key, entry.first, depth + 1, limit);
            if (!key_status.ok()) {
                return key_status;
            }
            if (!put_varint(out, static_cast<u64>(key.size())) ||
                !out.append(key.readable())) {
                return status{rpc_code::encode_error};
            }
            buffer mapped;
            const status value_status =
                encode_value(mapped, entry.second, depth + 1, limit);
            if (!value_status.ok()) {
                return value_status;
            }
            if (!put_varint(out, static_cast<u64>(mapped.size())) ||
                !out.append(mapped.readable())) {
                return status{rpc_code::encode_error};
            }
        }
        return status{};
    } else if constexpr (rtti::is_sequence<type>::value ||
                         rtti::is_fixed_sequence<type>::value) {
        if (depth > limit) {
            return status{rpc_code::encode_error};
        }
        if (!put_varint(out, static_cast<u64>(value.size()))) {
            return status{rpc_code::encode_error};
        }
        using element_t = typename type::value_type;
        for (const auto& element : value) {
            if constexpr (is_wire_scalar_v<element_t>) {
                const status element_status =
                    encode_value(out, element, depth + 1, limit);
                if (!element_status.ok()) {
                    return element_status;
                }
            } else {
                buffer encoded;
                const status element_status =
                    encode_value(encoded, element, depth + 1, limit);
                if (!element_status.ok()) {
                    return element_status;
                }
                if (!put_varint(out, static_cast<u64>(encoded.size())) ||
                    !out.append(encoded.readable())) {
                    return status{rpc_code::encode_error};
                }
            }
        }
        return status{};
    } else if constexpr (rtti::is_reflected<type>::value) {
        if (depth > limit) {
            return status{rpc_code::encode_error};
        }
        return encode_struct_fields(value, out, 0, depth, limit);
    } else {
        static_assert(dependent_false<type>::value,
                      "crpc-binary: unsupported field type; register the type "
                      "with RTTI_DEFINE_TYPE and use a supported container");
    }
}
#endif // CRPC_HAVE_RTTI

// --- binary decode -----------------------------------------------------------

#if CRPC_HAVE_RTTI
template <typename T>
status decode_value(byte_reader& reader, T& value, std::size_t depth,
                    std::size_t limit, bool strict);

template <typename T>
bool decode_signed(byte_reader& reader, T& value) {
    i64 raw = 0;
    if (!reader.get_varint_i64(raw)) {
        return false;
    }
    if (raw < static_cast<i64>(std::numeric_limits<T>::min()) ||
        raw > static_cast<i64>(std::numeric_limits<T>::max())) {
        return false;
    }
    value = static_cast<T>(raw);
    return true;
}

template <typename T>
bool decode_unsigned(byte_reader& reader, T& value) {
    u64 raw = 0;
    if (!reader.get_varint(raw)) {
        return false;
    }
    if (raw > static_cast<u64>(std::numeric_limits<T>::max())) {
        return false;
    }
    value = static_cast<T>(raw);
    return true;
}

template <typename T>
status decode_struct_fields(byte_reader& reader, T& value, u64 id_offset,
                            std::size_t depth, std::size_t limit, bool strict);


template <typename T, typename Members, std::size_t... I>
status decode_member_dispatch(u64 id, u64 id_offset, byte_reader& reader,
                              T& value, std::size_t depth, std::size_t limit,
                              bool strict, const Members& members,
                              bool& handled, std::index_sequence<I...>) {
    status result;
    const auto step = [&](auto index_constant) {
        constexpr std::size_t index = decltype(index_constant)::value;
        if (handled || !result.ok()) {
            return;
        }
        if (id == id_offset + index + 1) {
            handled = true;
            result = decode_value(reader, value.*std::get<index>(members),
                                  depth + 1, limit, strict);
        }
    };
    (step(std::integral_constant<std::size_t, I>{}), ...);
    return result;
}

template <typename T>
status decode_field_dispatch(u64 id, u64 id_offset, byte_reader& reader,
                             T& value, std::size_t depth, std::size_t limit,
                             bool strict, bool& handled) {
    const auto& members = flat_members<T>();
    using members_t = std::decay_t<decltype(members)>;
    return decode_member_dispatch(
        id, id_offset, reader, value, depth, limit, strict, members, handled,
        std::make_index_sequence<std::tuple_size<members_t>::value>{});
}

template <typename T>
status decode_struct_fields(byte_reader& reader, T& value, u64 id_offset,
                            std::size_t depth, std::size_t limit, bool strict) {
    if (depth > limit) {
        return status{rpc_code::decode_error};
    }
    if constexpr (rtti::has_default<T>::value) {
        value = T{};
    }

    constexpr std::size_t total = rtti::field_count<T>();
    std::vector<bool> seen(total + 1, false);

    while (reader.remaining() != 0) {
        u64 id = 0;
        if (!reader.get_varint(id)) {
            return status{rpc_code::decode_error};
        }
        if (id == 0) {
            return status{rpc_code::decode_error};
        }
        u64 length = 0;
        if (!reader.get_varint(length)) {
            return status{rpc_code::decode_error};
        }
        if (length > static_cast<u64>(reader.remaining())) {
            return status{rpc_code::decode_error};
        }
        span<const std::byte> payload;
        if (!reader.get_bytes(static_cast<std::size_t>(length), payload)) {
            return status{rpc_code::decode_error};
        }

        const bool known =
            id > id_offset && id <= id_offset + static_cast<u64>(total);
        if (known) {
            const std::size_t local = static_cast<std::size_t>(id - id_offset);
            if (seen[local]) {
                return status{rpc_code::decode_error};
            }
            seen[local] = true;
        }

        byte_reader payload_reader(payload);
        bool handled = false;
        const status dispatched = decode_field_dispatch(
            id, id_offset, payload_reader, value, depth, limit, strict, handled);
        if (!dispatched.ok()) {
            return dispatched;
        }
        if (!handled) {
            // Unknown field: the explicit length made it skippable. Forward
            // compatibility requires accepting it.
            continue;
        }
        if (strict && payload_reader.remaining() != 0) {
            return status{rpc_code::decode_error};
        }
    }
    return status{};
}

template <typename T>
status decode_value(byte_reader& reader, T& value, std::size_t depth,
                    std::size_t limit, bool strict) {
    using type = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<type, bool>) {
        u8 raw = 0;
        if (!reader.get_u8(raw) || raw > 1) {
            return status{rpc_code::decode_error};
        }
        value = raw != 0;
        return status{};
    } else if constexpr (std::is_enum_v<type>) {
        using underlying = std::underlying_type_t<type>;
        underlying raw{};
        const bool ok = std::is_signed_v<underlying>
                            ? decode_signed(reader, raw)
                            : decode_unsigned(reader, raw);
        if (!ok) {
            return status{rpc_code::decode_error};
        }
        value = static_cast<type>(raw);
        return status{};
    } else if constexpr (std::is_integral_v<type>) {
        const bool ok = std::is_signed_v<type> ? decode_signed(reader, value)
                                               : decode_unsigned(reader, value);
        return ok ? status{} : status{rpc_code::decode_error};
    } else if constexpr (std::is_floating_point_v<type>) {
        if constexpr (std::is_same_v<type, float>) {
            u32 bits = 0;
            if (!reader.get_u32(bits)) {
                return status{rpc_code::decode_error};
            }
            value = endian::bits_to_float(bits);
            return status{};
        } else if constexpr (std::is_same_v<type, double>) {
            u64 bits = 0;
            if (!reader.get_u64(bits)) {
                return status{rpc_code::decode_error};
            }
            value = endian::bits_to_double(bits);
            return status{};
        } else {
            static_assert(dependent_false<type>::value,
                          "crpc-binary: long double is not supported");
        }
    } else if constexpr (is_text<type>::value) {
        static_assert(!rtti::is_string_view<type>::value,
                      "crpc-binary: decoding into std::string_view is not "
                      "supported (the view would dangle); decode into "
                      "std::string instead");
        const span<const std::byte> bytes = reader.remaining_bytes();
        if (!bytes.empty()) {
            value.assign(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
        } else {
            value.clear();
        }
        if (!reader.skip(bytes.size())) {
            return status{rpc_code::decode_error};
        }
        return status{};
    } else if constexpr (is_byte_sequence<type>::value) {
        if constexpr (std::is_array_v<type>) {
            const span<const std::byte> bytes = reader.remaining_bytes();
            if (bytes.size() != value.size()) {
                return status{rpc_code::decode_error};
            }
            if (!bytes.empty()) {
                std::memcpy(value.data(), bytes.data(), bytes.size());
            }
            if (!reader.skip(bytes.size())) {
                return status{rpc_code::decode_error};
            }
            return status{};
        } else {
            const span<const std::byte> bytes = reader.remaining_bytes();
            value.resize(bytes.size());
            if (!bytes.empty()) {
                std::memcpy(value.data(), bytes.data(), bytes.size());
            }
            if (!reader.skip(bytes.size())) {
                return status{rpc_code::decode_error};
            }
            return status{};
        }
    } else if constexpr (rtti::is_optional<type>::value) {
        if (depth > limit) {
            return status{rpc_code::decode_error};
        }
        u8 presence = 0;
        if (!reader.get_u8(presence) || presence > 1) {
            return status{rpc_code::decode_error};
        }
        if (presence == 0) {
            value.reset();
            return status{};
        }
        value.emplace();
        return decode_value(reader, *value, depth + 1, limit, strict);
    } else if constexpr (rtti::is_map<type>::value) {
        if (depth > limit) {
            return status{rpc_code::decode_error};
        }
        u64 count = 0;
        if (!reader.get_varint(count)) {
            return status{rpc_code::decode_error};
        }
        if (count > static_cast<u64>(reader.remaining()) / 2u) {
            return status{rpc_code::decode_error};
        }
        value.clear();
        for (u64 i = 0; i < count; ++i) {
            u64 key_length = 0;
            if (!reader.get_varint(key_length)) {
                return status{rpc_code::decode_error};
            }
            if (key_length > static_cast<u64>(reader.remaining())) {
                return status{rpc_code::decode_error};
            }
            span<const std::byte> key_bytes;
            if (!reader.get_bytes(static_cast<std::size_t>(key_length),
                                  key_bytes)) {
                return status{rpc_code::decode_error};
            }
            typename type::key_type key{};
            byte_reader key_reader(key_bytes);
            const status key_status =
                decode_value(key_reader, key, depth + 1, limit, strict);
            if (!key_status.ok()) {
                return key_status;
            }
            if (strict && key_reader.remaining() != 0) {
                return status{rpc_code::decode_error};
            }

            u64 value_length = 0;
            if (!reader.get_varint(value_length)) {
                return status{rpc_code::decode_error};
            }
            if (value_length > static_cast<u64>(reader.remaining())) {
                return status{rpc_code::decode_error};
            }
            span<const std::byte> value_bytes;
            if (!reader.get_bytes(static_cast<std::size_t>(value_length),
                                  value_bytes)) {
                return status{rpc_code::decode_error};
            }
            typename type::mapped_type mapped{};
            byte_reader value_reader(value_bytes);
            const status value_status =
                decode_value(value_reader, mapped, depth + 1, limit, strict);
            if (!value_status.ok()) {
                return value_status;
            }
            if (strict && value_reader.remaining() != 0) {
                return status{rpc_code::decode_error};
            }
            value.insert_or_assign(std::move(key), std::move(mapped));
        }
        return status{};
    } else if constexpr (rtti::is_sequence<type>::value ||
                         rtti::is_fixed_sequence<type>::value) {
        if (depth > limit) {
            return status{rpc_code::decode_error};
        }
        u64 count = 0;
        if (!reader.get_varint(count)) {
            return status{rpc_code::decode_error};
        }
        if (count > static_cast<u64>(reader.remaining())) {
            return status{rpc_code::decode_error};
        }
        if constexpr (std::is_array_v<type>) {
            if (count != static_cast<u64>(value.size())) {
                return status{rpc_code::decode_error};
            }
        } else {
            value.clear();
        }
        using element_t = typename type::value_type;
        for (u64 i = 0; i < count; ++i) {
            if constexpr (is_wire_scalar_v<element_t>) {
                element_t element{};
                const status element_status =
                    decode_value(reader, element, depth + 1, limit, strict);
                if (!element_status.ok()) {
                    return element_status;
                }
                if constexpr (std::is_array_v<type>) {
                    value[static_cast<std::size_t>(i)] = element;
                } else {
                    value.push_back(std::move(element));
                }
            } else {
                u64 length = 0;
                if (!reader.get_varint(length)) {
                    return status{rpc_code::decode_error};
                }
                if (length > static_cast<u64>(reader.remaining())) {
                    return status{rpc_code::decode_error};
                }
                span<const std::byte> payload;
                if (!reader.get_bytes(static_cast<std::size_t>(length),
                                      payload)) {
                    return status{rpc_code::decode_error};
                }
                byte_reader element_reader(payload);
                element_t element{};
                const status element_status = decode_value(
                    element_reader, element, depth + 1, limit, strict);
                if (!element_status.ok()) {
                    return element_status;
                }
                if (strict && element_reader.remaining() != 0) {
                    return status{rpc_code::decode_error};
                }
                if constexpr (std::is_array_v<type>) {
                    value[static_cast<std::size_t>(i)] = std::move(element);
                } else {
                    value.push_back(std::move(element));
                }
            }
        }
        return status{};
    } else if constexpr (rtti::is_reflected<type>::value) {
        if (depth > limit) {
            return status{rpc_code::decode_error};
        }
        return decode_struct_fields(reader, value, 0, depth, limit, strict);
    } else {
        static_assert(dependent_false<type>::value,
                      "crpc-binary: unsupported field type; register the type "
                      "with RTTI_DEFINE_TYPE and use a supported container");
    }
}
#endif // CRPC_HAVE_RTTI

// --- erased type table -------------------------------------------------------

// The table type itself has no RTTI dependency: it is three function pointers
// keyed by std::type_info. Only the per-type instantiation below needs the
// reflection headers, which is why the struct stays outside the guard.
struct type_ops {
    const std::type_info* type;
    status (*encode)(const void*, buffer&, std::size_t depth_limit);
    status (*decode)(span<const std::byte>, void*, std::size_t depth_limit,
                     bool strict);
};

#if CRPC_HAVE_RTTI

template <typename T>
status encode_erased(const void* value, buffer& out, std::size_t depth_limit) {
    try {
        return encode_value(out, *static_cast<const T*>(value), 0, depth_limit);
    } catch (const std::bad_alloc&) {
        return status{rpc_code::resource_exhausted};
    } catch (const std::length_error&) {
        return status{rpc_code::resource_exhausted};
    }
}

template <typename T>
status decode_erased(span<const std::byte> in, void* value,
                     std::size_t depth_limit, bool strict) {
    try {
        byte_reader reader(in);
        const status decoded =
            decode_value(reader, *static_cast<T*>(value), 0, depth_limit,
                         strict);
        if (!decoded.ok()) {
            return decoded;
        }
        if (strict && reader.remaining() != 0) {
            return status{rpc_code::decode_error};
        }
        return status{};
    } catch (const std::bad_alloc&) {
        return status{rpc_code::resource_exhausted};
    } catch (const std::length_error&) {
        return status{rpc_code::resource_exhausted};
    }
}

template <typename T>
const type_ops* ops_for() noexcept {
#if CRPC_CAN_USE_TYPEID
    static const type_ops ops{&typeid(T), &encode_erased<T>, &decode_erased<T>};
    return &ops;
#else
    // The static_assert always fires, so the return is unreachable; it exists
    // only to keep the signature valid and the diagnostic actionable (a direct
    // caller sees this message instead of "no member named ops_for").
    static_assert(dependent_false<T>::value,
                  "crpc-binary: the erased type table requires compiler RTTI");
    return nullptr;
#endif
}
#endif // CRPC_HAVE_RTTI

// --- text-format error formatting -------------------------------------------

#if CRPC_HAVE_RTTI
inline std::string format_rtti_error(const char* format,
                                     const rtti::error_info& error) {
    std::string message = "rtti ";
    message += format;
    message += " error ";
    message += rtti::to_string(error.code);
    message += " at offset ";
    message += std::to_string(error.offset);
    return message;
}
#endif

} // namespace detail

// --- binary codec ------------------------------------------------------------

class binary_codec final : public codec {
public:
    static constexpr std::size_t default_depth_limit = 32;

    explicit binary_codec(std::size_t depth_limit = default_depth_limit,
                          bool strict = true) noexcept
        : depth_limit_(depth_limit), strict_(strict) {}

    binary_codec(const binary_codec&) = delete;
    binary_codec& operator=(const binary_codec&) = delete;

    [[nodiscard]] codec_id id() const noexcept override {
        return codec_id::crpc_binary;
    }
    [[nodiscard]] std::string_view content_type() const noexcept override {
        return "application/crpc";
    }

    [[nodiscard]] status encode(const void* value, const std::type_info& type,
                                buffer& out) const override {
        const detail::type_ops* ops = find_ops(type);
        if (ops == nullptr) {
            const rpc_error failure(
                rpc_code::encode_error,
                "crpc-binary: value type is not registered with "
                "binary_codec::register_type<T>()");
            return status{failure};
        }
        return ops->encode(value, out, depth_limit_);
    }

    [[nodiscard]] status decode(span<const std::byte> in, void* value,
                                const std::type_info& type) const override {
        const detail::type_ops* ops = find_ops(type);
        if (ops == nullptr) {
            const rpc_error failure(
                rpc_code::decode_error,
                "crpc-binary: value type is not registered with "
                "binary_codec::register_type<T>()");
            return status{failure};
        }
        return ops->decode(in, value, depth_limit_, strict_);
    }

    // Registers T for the erased encode/decode path. The typed helpers do not
    // require this. Registration is idempotent and thread-safe.
    template <typename T>
    void register_type() const {
#if CRPC_HAVE_RTTI && CRPC_CAN_USE_TYPEID
        const detail::type_ops* ops = detail::ops_for<T>();
        std::unique_lock<std::shared_mutex> lock(types_mutex_);
        types_.emplace(std::type_index(typeid(T)), ops);
#elif !CRPC_HAVE_RTTI
        static_assert(detail::dependent_false<T>::value,
                      "crpc-binary: register_type<T>() requires the RTTI "
                      "reflection dependency (CRPC_HAVE_RTTI)");
#else
        static_assert(detail::dependent_false<T>::value,
                      "crpc-binary: register_type<T>() requires compiler RTTI "
                      "(-fno-rtti)");
#endif
    }

    // Typed fast path: one compile-time field walk, no registry lookup.
    template <typename T>
    status encode_typed(const T& value, buffer& out) const {
#if CRPC_HAVE_RTTI
        try {
            return detail::encode_value(out, value, 0, depth_limit_);
        } catch (const std::bad_alloc&) {
            return status{rpc_code::resource_exhausted};
        } catch (const std::length_error&) {
            return status{rpc_code::resource_exhausted};
        }
#else
        (void)value;
        (void)out;
        static_assert(detail::dependent_false<T>::value,
                      "crpc-binary requires the RTTI dependency");
        return status{rpc_code::encode_error};
#endif
    }

    template <typename T>
    status decode_typed(span<const std::byte> in, T& value) const {
#if CRPC_HAVE_RTTI
        try {
            byte_reader reader(in);
            const status decoded =
                detail::decode_value(reader, value, 0, depth_limit_, strict_);
            if (!decoded.ok()) {
                return decoded;
            }
            if (strict_ && reader.remaining() != 0) {
                return status{rpc_code::decode_error};
            }
            return status{};
        } catch (const std::bad_alloc&) {
            return status{rpc_code::resource_exhausted};
        } catch (const std::length_error&) {
            return status{rpc_code::resource_exhausted};
        }
#else
        (void)in;
        (void)value;
        static_assert(detail::dependent_false<T>::value,
                      "crpc-binary requires the RTTI dependency");
        return status{rpc_code::decode_error};
#endif
    }

    [[nodiscard]] std::size_t depth_limit() const noexcept {
        return depth_limit_;
    }
    [[nodiscard]] bool strict() const noexcept { return strict_; }

private:
    [[nodiscard]] const detail::type_ops* find_ops(
        const std::type_info& type) const noexcept {
        std::shared_lock<std::shared_mutex> lock(types_mutex_);
        const auto it = types_.find(std::type_index(type));
        return it == types_.end() ? nullptr : it->second;
    }

    std::size_t depth_limit_;
    bool strict_;
    mutable std::shared_mutex types_mutex_;
    mutable std::unordered_map<std::type_index, const detail::type_ops*> types_;
};

// --- text format tags --------------------------------------------------------

#if CRPC_HAVE_RTTI_JSON
struct json_format {
    static constexpr codec_id id = codec_id::json;
    static constexpr std::string_view content_type = "application/json";
    static constexpr const char* name() noexcept { return "json"; }

    template <typename T>
    static std::string serialize(const T& value) {
        return rtti::json::serialize<T>(value);
    }
    template <typename T>
    static rtti::result<T> parse(rtti::string_view text) {
        return rtti::json::parse_as<T>(text);
    }
};
#endif

#if CRPC_HAVE_RTTI_XML
struct xml_format {
    static constexpr codec_id id = codec_id::xml;
    static constexpr std::string_view content_type = "application/xml";
    static constexpr const char* name() noexcept { return "xml"; }

    template <typename T>
    static std::string serialize(const T& value) {
        return rtti::xml::serialize<T>(value);
    }
    template <typename T>
    static rtti::result<T> parse(rtti::string_view text) {
        return rtti::xml::parse_as<T>(text);
    }
};
#endif

#if CRPC_HAVE_RTTI_INI
struct ini_format {
    static constexpr codec_id id = codec_id::ini;
    static constexpr std::string_view content_type = "application/ini";
    static constexpr const char* name() noexcept { return "ini"; }

    template <typename T>
    static std::string serialize(const T& value) {
        return rtti::ini::serialize<T>(value);
    }
    template <typename T>
    static rtti::result<T> parse(rtti::string_view text) {
        return rtti::ini::parse_as<T>(text);
    }
};
#endif

#if CRPC_HAVE_RTTI_TOML
struct toml_format {
    static constexpr codec_id id = codec_id::toml;
    static constexpr std::string_view content_type = "application/toml";
    static constexpr const char* name() noexcept { return "toml"; }

    template <typename T>
    static std::string serialize(const T& value) {
        return rtti::toml::serialize<T>(value);
    }
    template <typename T>
    static rtti::result<T> parse(rtti::string_view text) {
        return rtti::toml::parse_as<T>(text);
    }
};
#endif

#if CRPC_HAVE_RTTI_PROTOBUF
struct protobuf_format {
    static constexpr codec_id id = codec_id::protobuf;
    static constexpr std::string_view content_type = "application/x-protobuf";
    static constexpr const char* name() noexcept { return "protobuf"; }

    template <typename T>
    static std::string serialize(const T& value) {
        static_assert(rtti::proto::schema<T>::count > 0,
                      "crpc protobuf codec: specialize rtti::proto::schema<T> "
                      "for this type before using it");
        std::string out;
        if (rtti::proto::serialize(value, out) != rtti::error_code::ok) {
            return std::string();
        }
        return out;
    }
    template <typename T>
    static rtti::result<T> parse(rtti::string_view text) {
        static_assert(rtti::proto::schema<T>::count > 0,
                      "crpc protobuf codec: specialize rtti::proto::schema<T> "
                      "for this type before using it");
        return rtti::proto::parse_as<T>(text);
    }
};
#endif

#if CRPC_HAVE_RTTI_FLATBUFFERS
struct flatbuffers_format {
    static constexpr codec_id id = codec_id::flatbuffers;
    static constexpr std::string_view content_type =
        "application/x-flatbuffers";
    static constexpr const char* name() noexcept { return "flatbuffers"; }

    template <typename T>
    static std::string serialize(const T& value) {
        static_assert(rtti::flat::schema<T>::count > 0,
                      "crpc FlatBuffers codec: specialize rtti::flat::schema<T> "
                      "for this type before using it");
        std::string out;
        if (rtti::flat::serialize(value, out) != rtti::error_code::ok) {
            return std::string();
        }
        return out;
    }
    template <typename T>
    static rtti::result<T> parse(rtti::string_view text) {
        static_assert(rtti::flat::schema<T>::count > 0,
                      "crpc FlatBuffers codec: specialize rtti::flat::schema<T> "
                      "for this type before using it");
        return rtti::flat::parse_as<T>(text, rtti::flat::parse_config{});
    }
};
#endif

// --- generic text codec ------------------------------------------------------

#if CRPC_HAVE_RTTI
namespace detail {

// Erased entry points for the text formats. They are separate from the binary
// ops_for<T>() table: the erased `codec::encode`/`decode` of a text codec must
// run that format's conversion, not the binary walk.
template <typename Format, typename T>
status encode_text_erased(const void* value, buffer& out, std::size_t) {
    try {
        const std::string text =
            Format::template serialize<T>(*static_cast<const T*>(value));
        if (text.empty()) {
            const rpc_error failure(
                rpc_code::encode_error,
                "text codec: RTTI conversion failed (empty document)");
            return status{failure};
        }
        if (!out.append(text.data(), text.size())) {
            return status{rpc_code::encode_error};
        }
        return status{};
    } catch (const std::bad_alloc&) {
        return status{rpc_code::resource_exhausted};
    } catch (const std::length_error&) {
        return status{rpc_code::resource_exhausted};
    }
}

template <typename Format, typename T>
status decode_text_erased(span<const std::byte> in, void* value, std::size_t,
                          bool) {
    try {
        const rtti::string_view text(reinterpret_cast<const char*>(in.data()),
                                     in.size());
        rtti::result<T> parsed = Format::template parse<T>(text);
        if (!parsed.ok()) {
            const rpc_error failure(
                rpc_code::decode_error,
                format_rtti_error(Format::name(), parsed.err));
            return status{failure};
        }
        *static_cast<T*>(value) = std::move(parsed.value);
        return status{};
    } catch (const std::bad_alloc&) {
        return status{rpc_code::resource_exhausted};
    } catch (const std::length_error&) {
        return status{rpc_code::resource_exhausted};
    }
}

template <typename Format, typename T>
const type_ops* text_ops_for() noexcept {
#if CRPC_CAN_USE_TYPEID
    static const type_ops ops{&typeid(T), &encode_text_erased<Format, T>,
                              &decode_text_erased<Format, T>};
    return &ops;
#else
    // Unreachable: the static_assert always fires. The return keeps the
    // signature valid and the diagnostic actionable.
    static_assert(dependent_false<T>::value,
                  "text codec: the erased type table requires compiler RTTI");
    return nullptr;
#endif
}

} // namespace detail

template <typename Format>
class text_codec final : public codec {
public:
    [[nodiscard]] codec_id id() const noexcept override { return Format::id; }
    [[nodiscard]] std::string_view content_type() const noexcept override {
        return Format::content_type;
    }

    [[nodiscard]] status encode(const void* value, const std::type_info& type,
                                buffer& out) const override {
        const detail::type_ops* ops = find_ops(type);
        if (ops == nullptr) {
            const rpc_error failure(
                rpc_code::encode_error,
                "text codec: value type is not registered with "
                "register_type<T>()");
            return status{failure};
        }
        return ops->encode(value, out, 0);
    }

    [[nodiscard]] status decode(span<const std::byte> in, void* value,
                                const std::type_info& type) const override {
        const detail::type_ops* ops = find_ops(type);
        if (ops == nullptr) {
            const rpc_error failure(
                rpc_code::decode_error,
                "text codec: value type is not registered with "
                "register_type<T>()");
            return status{failure};
        }
        return ops->decode(in, value, 0, true);
    }

    template <typename T>
    void register_type() const {
#if CRPC_CAN_USE_TYPEID
        const detail::type_ops* ops = detail::text_ops_for<Format, T>();
        std::unique_lock<std::shared_mutex> lock(types_mutex_);
        types_.emplace(std::type_index(typeid(T)), ops);
#else
        static_assert(detail::dependent_false<T>::value,
                      "text codec: register_type<T>() requires compiler RTTI "
                      "(-fno-rtti)");
#endif
    }

    template <typename T>
    status encode_typed(const T& value, buffer& out) const {
        try {
            const std::string text = Format::template serialize<T>(value);
            if (text.empty()) {
                const rpc_error failure(
                    rpc_code::encode_error,
                    "text codec: RTTI conversion failed (empty document)");
                return status{failure};
            }
            if (!out.append(text.data(), text.size())) {
                return status{rpc_code::encode_error};
            }
            return status{};
        } catch (const std::bad_alloc&) {
            return status{rpc_code::resource_exhausted};
        } catch (const std::length_error&) {
            return status{rpc_code::resource_exhausted};
        }
    }

    template <typename T>
    status decode_typed(span<const std::byte> in, T& value) const {
        try {
            const rtti::string_view text(
                reinterpret_cast<const char*>(in.data()), in.size());
            rtti::result<T> parsed = Format::template parse<T>(text);
            if (!parsed.ok()) {
                const rpc_error failure(
                    rpc_code::decode_error,
                    detail::format_rtti_error(Format::name(), parsed.err));
                return status{failure};
            }
            value = std::move(parsed.value);
            return status{};
        } catch (const std::bad_alloc&) {
            return status{rpc_code::resource_exhausted};
        } catch (const std::length_error&) {
            return status{rpc_code::resource_exhausted};
        }
    }

private:
    [[nodiscard]] const detail::type_ops* find_ops(
        const std::type_info& type) const noexcept {
        std::shared_lock<std::shared_mutex> lock(types_mutex_);
        const auto it = types_.find(std::type_index(type));
        return it == types_.end() ? nullptr : it->second;
    }

    mutable std::shared_mutex types_mutex_;
    mutable std::unordered_map<std::type_index, const detail::type_ops*> types_;
};

#if CRPC_HAVE_RTTI_JSON
using json_codec = text_codec<json_format>;
#endif
#if CRPC_HAVE_RTTI_XML
using xml_codec = text_codec<xml_format>;
#endif
#if CRPC_HAVE_RTTI_INI
using ini_codec = text_codec<ini_format>;
#endif
#if CRPC_HAVE_RTTI_TOML
using toml_codec = text_codec<toml_format>;
#endif
#if CRPC_HAVE_RTTI_PROTOBUF
using protobuf_codec = text_codec<protobuf_format>;
#endif
#if CRPC_HAVE_RTTI_FLATBUFFERS
using flatbuffers_codec = text_codec<flatbuffers_format>;
#endif
#endif // CRPC_HAVE_RTTI

// --- codec registry ----------------------------------------------------------

class codec_registry {
public:
    codec_registry() = default;
    ~codec_registry();

    codec_registry(const codec_registry&) = delete;
    codec_registry& operator=(const codec_registry&) = delete;

    // Adds a codec. Fails when the registry is frozen, when the id is 0, or
    // when the id or content type is already registered.
    [[nodiscard]] status add(std::unique_ptr<codec> entry);

    // Lookup is lock-free after freeze(). Before freeze() it takes a shared
    // lock, so concurrent registration and lookup are safe.
    [[nodiscard]] const codec* find(codec_id id) const noexcept;
    [[nodiscard]] const codec* find(std::string_view content_type) const noexcept;

    // Freezes the registry. Idempotent. After this call add() fails.
    [[nodiscard]] status freeze();
    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct snapshot;

    void rollback(const codec* raw, u16 key) noexcept;

    mutable std::shared_mutex mutex_;
    bool frozen_ = false;
    std::unordered_map<u16, const codec*> by_id_;
    std::vector<std::pair<std::string, const codec*>> by_content_type_;
    std::vector<std::unique_ptr<codec>> owned_;
    std::atomic<const snapshot*> snapshot_{nullptr};
    snapshot* snapshot_owned_ = nullptr;
};

// The process-wide default registry: crpc-binary plus every available RTTI
// format, frozen on first use.
[[nodiscard]] codec_registry& default_codec_registry();

// --- typed helpers -----------------------------------------------------------

// Zero-cost typed dispatch: one id() check, then the concrete codec's member
// template. No std::function and no type-erased callback.
//
// The cast is safe because the id/content-type registry maps each built-in id
// to exactly one built-in codec type. A custom codec registered under a
// different id uses the erased `codec::encode`/`decode` path below.
template <typename T>
[[nodiscard]] status encode_into(const codec& target, const T& value,
                                 buffer& out) {
    switch (target.id()) {
    case codec_id::crpc_binary:
        return static_cast<const binary_codec&>(target).encode_typed(value, out);
#if CRPC_HAVE_RTTI_JSON
    case codec_id::json:
        return static_cast<const json_codec&>(target).encode_typed(value, out);
#endif
#if CRPC_HAVE_RTTI_XML
    case codec_id::xml:
        return static_cast<const xml_codec&>(target).encode_typed(value, out);
#endif
#if CRPC_HAVE_RTTI_INI
    case codec_id::ini:
        return static_cast<const ini_codec&>(target).encode_typed(value, out);
#endif
#if CRPC_HAVE_RTTI_TOML
    case codec_id::toml:
        return static_cast<const toml_codec&>(target).encode_typed(value, out);
#endif
#if CRPC_HAVE_RTTI_PROTOBUF
    case codec_id::protobuf:
        if constexpr (rtti::proto::schema<T>::count > 0) {
            return static_cast<const protobuf_codec&>(target).encode_typed(
                value, out);
        } else {
            const rpc_error failure(
                rpc_code::encode_error,
                "crpc protobuf codec: specialize rtti::proto::schema<T> for "
                "this type before using it");
            return status{failure};
        }
#endif
#if CRPC_HAVE_RTTI_FLATBUFFERS
    case codec_id::flatbuffers:
        if constexpr (rtti::flat::schema<T>::count > 0) {
            return static_cast<const flatbuffers_codec&>(target).encode_typed(
                value, out);
        } else {
            const rpc_error failure(
                rpc_code::encode_error,
                "crpc FlatBuffers codec: specialize rtti::flat::schema<T> for "
                "this type before using it");
            return status{failure};
        }
#endif
    default:
        break;
    }
#if CRPC_CAN_USE_TYPEID
    // A custom codec registered under a non-built-in id: fall back to the
    // type-erased boundary, which is keyed by std::type_info.
    return target.encode(&value, typeid(T), out);
#else
    // This translation unit cannot form std::type_info, so the type-erased
    // dispatch does not exist. Report a precondition failure -- the build is
    // missing a capability, the payload is not at fault -- instead of silently
    // succeeding.
    (void)value;
    (void)out;
    const rpc_error failure(
        rpc_code::failed_precondition,
        "crpc: type-erased codec dispatch requires compiler RTTI; this build "
        "has none (-fno-rtti)");
    return status{failure};
#endif
}

template <typename T>
[[nodiscard]] status decode_from(const codec& source,
                                 span<const std::byte> in, T& value) {
    switch (source.id()) {
    case codec_id::crpc_binary:
        return static_cast<const binary_codec&>(source).decode_typed(in, value);
#if CRPC_HAVE_RTTI_JSON
    case codec_id::json:
        return static_cast<const json_codec&>(source).decode_typed(in, value);
#endif
#if CRPC_HAVE_RTTI_XML
    case codec_id::xml:
        return static_cast<const xml_codec&>(source).decode_typed(in, value);
#endif
#if CRPC_HAVE_RTTI_INI
    case codec_id::ini:
        return static_cast<const ini_codec&>(source).decode_typed(in, value);
#endif
#if CRPC_HAVE_RTTI_TOML
    case codec_id::toml:
        return static_cast<const toml_codec&>(source).decode_typed(in, value);
#endif
#if CRPC_HAVE_RTTI_PROTOBUF
    case codec_id::protobuf:
        if constexpr (rtti::proto::schema<T>::count > 0) {
            return static_cast<const protobuf_codec&>(source).decode_typed(
                in, value);
        } else {
            const rpc_error failure(
                rpc_code::decode_error,
                "crpc protobuf codec: specialize rtti::proto::schema<T> for "
                "this type before using it");
            return status{failure};
        }
#endif
#if CRPC_HAVE_RTTI_FLATBUFFERS
    case codec_id::flatbuffers:
        if constexpr (rtti::flat::schema<T>::count > 0) {
            return static_cast<const flatbuffers_codec&>(source).decode_typed(
                in, value);
        } else {
            const rpc_error failure(
                rpc_code::decode_error,
                "crpc FlatBuffers codec: specialize rtti::flat::schema<T> for "
                "this type before using it");
            return status{failure};
        }
#endif
    default:
        break;
    }
#if CRPC_CAN_USE_TYPEID
    // See encode_into above: the erased boundary needs std::type_info.
    return source.decode(in, &value, typeid(T));
#else
    (void)in;
    (void)value;
    const rpc_error failure(
        rpc_code::failed_precondition,
        "crpc: type-erased codec dispatch requires compiler RTTI; this build "
        "has none (-fno-rtti)");
    return status{failure};
#endif
}

} // namespace crpc

#endif // CRPC_CODEC_HPP





