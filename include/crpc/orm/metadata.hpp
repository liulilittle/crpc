// CRPC -- entity metadata built from reflection plus annotations.
//
// `entity_metadata<T>()` assembles one runtime table per entity type from two
// RTTI sources: the reflected member list (`fields_of<T>`: names and, via
// `std::get<I>(fields_of<T>::members)`, the exact member-pointer type of each
// field) and the annotation attribute tables registered by CRPC_ORM_ENTITY.
//
// The interesting trick is that the member-pointer tuple keeps the static type
// of every field on this side of the type erase. The per-field codec below is
// built as a `std::function` that captures the typed member pointer; the
// session layer only ever sees the erased `const void*` object plus a column
// index, so insert/update/select machinery can live in one non-template source
// file while each individual conversion stays fully typed.
#ifndef CRPC_ORM_METADATA_HPP
#define CRPC_ORM_METADATA_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <rtti/reflection.hpp>

#include <crpc/config.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/orm/fwd.hpp>
#include <crpc/orm/value.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace orm {

// One reflected field mapped to one database column. `index` is the ordinal in
// the field table; it is also the cursor column index of every query this ORM
// generates, because selection always follows entity column order.
struct column_meta {
    std::size_t index = orm_npos;
    std::string field;   // C++ member name
    std::string column;  // database column name (unquoted)
    db_type type = db_type::unknown;
    bool optional_field = false;  // the field is a std::optional<U>
    bool nullable = true;
    bool primary_key = false;
    bool auto_increment = false;
    std::uint32_t length = 0;
    std::uint32_t precision = 0;
    std::uint32_t scale = 0;

    // Type-erased codec bound to the reflected member.
    std::function<status(const void* obj, parameter& out)> encode;
    std::function<status(void* obj, result_set& set, std::size_t col)> decode;
    std::function<status(void* obj, const db_value& value)> decode_value;
    std::function<bool(const void* a, const void* b)> differs;
    std::function<void(void* dst, const void* src)> copy;
};

struct entity_meta {
    std::string table;
    std::vector<column_meta> columns;
    // Indices into `columns`, in field order.
    std::vector<std::size_t> primary_key;
    // The single auto-increment primary key column, or orm_npos.
    std::size_t auto_increment_index = orm_npos;

    bool valid = true;
    std::string error;
};

// Applies the ORM attribute set to a column_meta. Unknown attribute names are
// ignored for forward compatibility.
CRPC_API void apply_column_attributes(column_meta& column,
                                      span<const rtti::attr> attributes);

// Computes primary-key/auto-increment indices and validates the table shape.
CRPC_API void finalize_entity_metadata(entity_meta& meta);

namespace detail {

template <typename P>
struct member_traits {
    using class_type = void;
    using member_type = void;
};

template <typename T, typename M>
struct member_traits<M T::*> {
    using class_type = T;
    using member_type = M;
};

template <typename T, typename M>
struct member_codecs {
    static status encode(const void* obj, M T::* ptr, parameter& out) {
        const T& self = *static_cast<const T*>(obj);
        return values::encode_value(self.*ptr, out);
    }
    static status decode(void* obj, M T::* ptr, result_set& set, std::size_t col) {
        T& self = *static_cast<T*>(obj);
        return values::decode_cell(set, col, self.*ptr);
    }
    static status decode_value(void* obj, M T::* ptr, const db_value& value) {
        T& self = *static_cast<T*>(obj);
        return values::decode_value_into(value, self.*ptr);
    }
    static bool differs(const void* a, const void* b, M T::* ptr) {
        const T& lhs = *static_cast<const T*>(a);
        const T& rhs = *static_cast<const T*>(b);
        return !values::equal_value(lhs.*ptr, rhs.*ptr);
    }
    static void copy(void* dst, const void* src, M T::* ptr) {
        T& target = *static_cast<T*>(dst);
        const T& source = *static_cast<const T*>(src);
        target.*ptr = source.*ptr;
    }
};

template <typename T, std::size_t I>
struct field_of {
    static constexpr std::size_t index = I;
    // The stored member-pointer tuple is const, so std::get yields a
    // `const M T::* &`; strip cv and reference before the member_traits lookup.
    using member_ptr = typename std::remove_cv<typename std::remove_reference<
        decltype(std::get<I>(rtti::fields_of<T>::members))>::type>::type;
    using member_type = typename detail::member_traits<member_ptr>::member_type;
    static_assert(values::is_supported<member_type>::value,
                  "crpc::orm: a reflected member has an unsupported field type "
                  "(supported: bool, integral, scoped enum, floating point, "
                  "std::string, std::chrono::system_clock::time_point, "
                  "std::vector<std::byte> and std::optional of any of those)");
};

// Applies the ORM attribute set to a column_meta. Kept separate so metadata
// construction is easy to test without the reflection machinery.
template <typename T, std::size_t I>
column_meta make_column() {
    using member = field_of<T, I>;
    const rtti::field_access_base* accessor = rtti::fields<T>()[I];

    column_meta column;
    column.index = I;
    column.field = accessor->name;
    column.column = accessor->name;
    column.type = values::type_of_field<typename member::member_type>();
    column.optional_field = values::is_optional_field<typename member::member_type>::value;

    const rtti::field_attr_list attrs = rtti::field_attr<T>(column.field);
    if (attrs.count > 0 && attrs.items != nullptr) {
        apply_column_attributes(column, span<const rtti::attr>(attrs.items, attrs.count));
    }

    const typename member::member_ptr ptr = std::get<I>(rtti::fields_of<T>::members);
    using codec = member_codecs<T, typename member::member_type>;
    column.encode = [ptr](const void* obj, parameter& out) -> status {
        return codec::encode(obj, ptr, out);
    };
    column.decode = [ptr](void* obj, result_set& set, std::size_t col) -> status {
        return codec::decode(obj, ptr, set, col);
    };
    column.decode_value = [ptr](void* obj, const db_value& value) -> status {
        return codec::decode_value(obj, ptr, value);
    };
    column.differs = [ptr](const void* a, const void* b) -> bool {
        return codec::differs(a, b, ptr);
    };
    column.copy = [ptr](void* dst, const void* src) { codec::copy(dst, src, ptr); };
    return column;
}

template <typename T, std::size_t... Is>
entity_meta assemble_meta(std::index_sequence<Is...>) {
    entity_meta meta;
    (meta.columns.push_back(make_column<T, Is>()), ...);
    // The type-level `table` annotation (CRPC_ORM_ENTITY's first argument).
    if (const rtti::attr* table = rtti::find_attr<T>("table");
        table != nullptr && !table->args.empty()) {
        meta.table.assign(table->args[0].as_string().data(),
                          table->args[0].as_string().size());
    }
    finalize_entity_metadata(meta);
    return meta;
}

template <typename T, std::size_t... Is>
constexpr bool members_supported(std::index_sequence<Is...>) {
    return (values::is_supported<typename field_of<T, Is>::member_type>::value && ...);
}

} // namespace detail

// The metadata table for T, built once. Requires T to be registered with
// CRPC_ORM_ENTITY (or the equivalent RTTI registrations).
template <typename T>
const entity_meta* entity_metadata() {
    static_assert(detail::members_supported<T>(
                      std::make_index_sequence<rtti::field_count<T>()>()),
                  "crpc::orm: an entity has a reflected member with an unsupported "
                  "field type");
    static const entity_meta meta = detail::assemble_meta<T>(
        std::make_index_sequence<rtti::field_count<T>()>());
    return &meta;
}

// Strips cv-qualifiers and the reference a const tuple element comes back in.
template <typename Stored>
struct strip_stored {
    using type = typename std::remove_cv<typename std::remove_reference<Stored>::type>::type;
};

// Compares a caller-supplied member pointer with a reflected member's pointer.
// The comparison is only compiled when the types match: comparing unrelated
// member-pointer types is ill-formed, so the mismatch overload exists purely to
// make the fold well-formed and evaluate to false.
template <typename P, typename Stored>
inline auto member_ptr_equals(const P& member, const Stored& other)
    -> std::enable_if_t<std::is_same<P, typename strip_stored<Stored>::type>::value, bool> {
    return member == other;
}
template <typename P, typename Stored>
inline auto member_ptr_equals(const P&, const Stored&)
    -> std::enable_if_t<!std::is_same<P, typename strip_stored<Stored>::type>::value, bool> {
    return false;
}

// Resolves a reflected member pointer (e.g. &account::email) to its field
// ordinal. Returns orm_npos when no field of T matches.
template <typename T, typename P, std::size_t... Is>
inline std::size_t member_field_index_impl(P member, std::index_sequence<Is...>) {
    std::size_t found = orm_npos;
    ((void)(member_ptr_equals(member, std::get<Is>(rtti::fields_of<T>::members))
                ? (found = Is, 0)
                : 0),
     ...);
    return found;
}

template <typename T, typename P>
inline std::size_t member_field_index(P member) {
    return member_field_index_impl<T>(
        member, std::make_index_sequence<rtti::field_count<T>()>());
}

// Decodes every column of the current row into `out`. The SELECT statements
// this ORM generates list columns in entity order, so cursor column `i`
// corresponds to entity column `i`; a mismatch is a decode error rather than a
// silent offset.
template <typename T>
status decode_row(const entity_meta& meta, result_set& set, T& out) {
    if (set.column_count() != meta.columns.size()) {
        return status{rpc_code::decode_error,
                      "result column count does not match the entity column count"};
    }
    for (const column_meta& column : meta.columns) {
        const status decoded = column.decode(&out, set, column.index);
        if (!decoded) {
            return decoded;
        }
    }
    return status{};
}

} // namespace orm
} // namespace crpc

#endif // CRPC_ORM_METADATA_HPP