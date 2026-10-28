// CRPC -- entity annotations.
//
// How an entity declares its database shape:
//
//     struct account {
//         std::int64_t id;
//         std::string email;
//         std::optional<std::int64_t> age;
//         account_role role;              // a scoped enum
//     };
//
//     CRPC_ORM_ENTITY(account, "accounts",
//         CRPC_ORM_FIELD(id, CRPC_ORM_PK, CRPC_ORM_AUTO),
//         CRPC_ORM_FIELD(email, CRPC_ORM_NOT_NULL, CRPC_ORM_LENGTH(128)),
//         CRPC_ORM_FIELD(age, CRPC_ORM_NULLABLE),
//         CRPC_ORM_FIELD(role));
//
// One macro registers the type with the RTTI reflection machinery
// (`fields_of<T>`: the field table and the ordered tuple of typed member
// pointers) AND with the per-field attribute table that carries the ORM-only
// metadata (column name, primary key, auto increment, nullability, length).
//
// Why attributes over traits specializations (the two options offered for the
// annotation mechanism): attributes live next to the field list, so a rename of
// a member is a one-line change instead of a coordinator edit in a separate
// trait, and the attribute table is the same mechanism the underlying runtime
// already uses for its own annotations, so the ORM adds no new registration
// surface. Field name and column name are separate values, which is exactly the
// case a trait-based system makes most error prone.
//
// The annotation layer is a thin wrapper over RTTI's own macros; every
// expansion runs at namespace scope (like RTTI_DEFINE_TYPE itself). The field
// list is parenthesized tuples so commas inside a template argument or an
// attribute argument never reach the preprocessor's argument splitter.
#ifndef CRPC_ORM_ANNOTATE_HPP
#define CRPC_ORM_ANNOTATE_HPP

#include <rtti/reflection.hpp>

// --- Per-field attribute shorthands -----------------------------------------
// Each expands to the RTTI attribute this ORM looks up in `entity_metadata`.

#define CRPC_ORM_PK RTTI_ATTR("pk", 1)
#define CRPC_ORM_AUTO RTTI_ATTR("auto_increment", 1)
#define CRPC_ORM_NULLABLE RTTI_ATTR("nullable", 1)
#define CRPC_ORM_NOT_NULL RTTI_ATTR("nullable", 0)
#define CRPC_ORM_COLUMN(name) RTTI_ATTR("column", name)
#define CRPC_ORM_LENGTH(n) RTTI_ATTR("length", n)
#define CRPC_ORM_PRECISION(p) RTTI_ATTR("precision", p)
#define CRPC_ORM_SCALE(s) RTTI_ATTR("scale", s)

// One field entry: a parenthesized tuple `(member, attrs...)`.
#define CRPC_ORM_FIELD(...) (__VA_ARGS__)

// --- Tuple helpers ----------------------------------------------------------

#define CRPC_ORM_FIELD_NAME_IMPL(name, ...) name
#define CRPC_ORM_FIELD_NAME(tuple) CRPC_ORM_FIELD_NAME_IMPL tuple

// One field-attribute entry, WITHOUT a trailing comma: the entry list is
// comma-joined by per-count macros below, so RTTI_NARG (which counts macro
// arguments) sees exactly one entry per field. RTTI_APPLY_OPT cannot be used
// for the entry list — it mistakes a parenthesised-tuple list for an empty one.
#define CRPC_ORM_ENTRY_IMPL(name, ...) RTTI_FIELD_ATTR_ENTRY(name, __VA_ARGS__)
#define CRPC_ORM_ENTRY(T, tuple) CRPC_ORM_ENTRY_IMPL tuple

// --- Field-name joining (comma separated, no trailing comma) ----------------

#define CRPC_ORM_FN_0(T)
#define CRPC_ORM_FN_1(T, a1) CRPC_ORM_FIELD_NAME(a1)
#define CRPC_ORM_FN_2(T, a1, a2) CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2)
#define CRPC_ORM_FN_3(T, a1, a2, a3) CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3)
#define CRPC_ORM_FN_4(T, a1, a2, a3, a4) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4)
#define CRPC_ORM_FN_5(T, a1, a2, a3, a4, a5) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5)
#define CRPC_ORM_FN_6(T, a1, a2, a3, a4, a5, a6) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6)
#define CRPC_ORM_FN_7(T, a1, a2, a3, a4, a5, a6, a7) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7)
#define CRPC_ORM_FN_8(T, a1, a2, a3, a4, a5, a6, a7, a8) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8)
#define CRPC_ORM_FN_9(T, a1, a2, a3, a4, a5, a6, a7, a8, a9) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9)
#define CRPC_ORM_FN_10(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10)
#define CRPC_ORM_FN_11(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11)
#define CRPC_ORM_FN_12(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12)
#define CRPC_ORM_FN_13(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13)
#define CRPC_ORM_FN_14(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14)
#define CRPC_ORM_FN_15(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14), CRPC_ORM_FIELD_NAME(a15)
#define CRPC_ORM_FN_16(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14), CRPC_ORM_FIELD_NAME(a15), CRPC_ORM_FIELD_NAME(a16)
#define CRPC_ORM_FN_17(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14), CRPC_ORM_FIELD_NAME(a15), CRPC_ORM_FIELD_NAME(a16), \
    CRPC_ORM_FIELD_NAME(a17)
#define CRPC_ORM_FN_18(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14), CRPC_ORM_FIELD_NAME(a15), CRPC_ORM_FIELD_NAME(a16), \
    CRPC_ORM_FIELD_NAME(a17), CRPC_ORM_FIELD_NAME(a18)
#define CRPC_ORM_FN_19(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14), CRPC_ORM_FIELD_NAME(a15), CRPC_ORM_FIELD_NAME(a16), \
    CRPC_ORM_FIELD_NAME(a17), CRPC_ORM_FIELD_NAME(a18), CRPC_ORM_FIELD_NAME(a19)
#define CRPC_ORM_FN_20(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20) \
    CRPC_ORM_FIELD_NAME(a1), CRPC_ORM_FIELD_NAME(a2), CRPC_ORM_FIELD_NAME(a3), CRPC_ORM_FIELD_NAME(a4), \
    CRPC_ORM_FIELD_NAME(a5), CRPC_ORM_FIELD_NAME(a6), CRPC_ORM_FIELD_NAME(a7), CRPC_ORM_FIELD_NAME(a8), \
    CRPC_ORM_FIELD_NAME(a9), CRPC_ORM_FIELD_NAME(a10), CRPC_ORM_FIELD_NAME(a11), CRPC_ORM_FIELD_NAME(a12), \
    CRPC_ORM_FIELD_NAME(a13), CRPC_ORM_FIELD_NAME(a14), CRPC_ORM_FIELD_NAME(a15), CRPC_ORM_FIELD_NAME(a16), \
    CRPC_ORM_FIELD_NAME(a17), CRPC_ORM_FIELD_NAME(a18), CRPC_ORM_FIELD_NAME(a19), CRPC_ORM_FIELD_NAME(a20)

// --- Field-attribute entry joining (comma separated, no trailing comma) ------
// Mirrors the name join above; each item is a full RTTI_FIELD_ATTR_ENTRY call.

#define CRPC_ORM_ENTRIES_1(T, a1) CRPC_ORM_ENTRY(T, a1)
#define CRPC_ORM_ENTRIES_2(T, a1, a2) CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2)
#define CRPC_ORM_ENTRIES_3(T, a1, a2, a3) CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3)
#define CRPC_ORM_ENTRIES_4(T, a1, a2, a3, a4) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4)
#define CRPC_ORM_ENTRIES_5(T, a1, a2, a3, a4, a5) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5)
#define CRPC_ORM_ENTRIES_6(T, a1, a2, a3, a4, a5, a6) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6)
#define CRPC_ORM_ENTRIES_7(T, a1, a2, a3, a4, a5, a6, a7) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7)
#define CRPC_ORM_ENTRIES_8(T, a1, a2, a3, a4, a5, a6, a7, a8) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8)
#define CRPC_ORM_ENTRIES_9(T, a1, a2, a3, a4, a5, a6, a7, a8, a9) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9)
#define CRPC_ORM_ENTRIES_10(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10)
#define CRPC_ORM_ENTRIES_11(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11)
#define CRPC_ORM_ENTRIES_12(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12)
#define CRPC_ORM_ENTRIES_13(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13)
#define CRPC_ORM_ENTRIES_14(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14)
#define CRPC_ORM_ENTRIES_15(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14), CRPC_ORM_ENTRY(T, a15)
#define CRPC_ORM_ENTRIES_16(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14), CRPC_ORM_ENTRY(T, a15), CRPC_ORM_ENTRY(T, a16)
#define CRPC_ORM_ENTRIES_17(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14), CRPC_ORM_ENTRY(T, a15), CRPC_ORM_ENTRY(T, a16), \
    CRPC_ORM_ENTRY(T, a17)
#define CRPC_ORM_ENTRIES_18(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14), CRPC_ORM_ENTRY(T, a15), CRPC_ORM_ENTRY(T, a16), \
    CRPC_ORM_ENTRY(T, a17), CRPC_ORM_ENTRY(T, a18)
#define CRPC_ORM_ENTRIES_19(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14), CRPC_ORM_ENTRY(T, a15), CRPC_ORM_ENTRY(T, a16), \
    CRPC_ORM_ENTRY(T, a17), CRPC_ORM_ENTRY(T, a18), CRPC_ORM_ENTRY(T, a19)
#define CRPC_ORM_ENTRIES_20(T, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20) \
    CRPC_ORM_ENTRY(T, a1), CRPC_ORM_ENTRY(T, a2), CRPC_ORM_ENTRY(T, a3), CRPC_ORM_ENTRY(T, a4), \
    CRPC_ORM_ENTRY(T, a5), CRPC_ORM_ENTRY(T, a6), CRPC_ORM_ENTRY(T, a7), CRPC_ORM_ENTRY(T, a8), \
    CRPC_ORM_ENTRY(T, a9), CRPC_ORM_ENTRY(T, a10), CRPC_ORM_ENTRY(T, a11), CRPC_ORM_ENTRY(T, a12), \
    CRPC_ORM_ENTRY(T, a13), CRPC_ORM_ENTRY(T, a14), CRPC_ORM_ENTRY(T, a15), CRPC_ORM_ENTRY(T, a16), \
    CRPC_ORM_ENTRY(T, a17), CRPC_ORM_ENTRY(T, a18), CRPC_ORM_ENTRY(T, a19), CRPC_ORM_ENTRY(T, a20)

// --- Entity registration ----------------------------------------------------
//
// The field count comes from RTTI_NARG, which counts the parenthesised tuples
// correctly. RTTI_IS_EMPTY cannot be used here: it answers "does this argument
// list begin with a left parenthesis", and a tuple list always does, so it
// would report a non-empty entity as empty.

#define CRPC_ORM_FN_OPT(T, ...) \
    RTTI_EXPAND(RTTI_CAT(CRPC_ORM_FN_, RTTI_NARG(__VA_ARGS__))(T, __VA_ARGS__))

#define CRPC_ORM_ENTRIES_OPT(T, ...) \
    RTTI_EXPAND(RTTI_CAT(CRPC_ORM_ENTRIES_, RTTI_NARG(__VA_ARGS__))(T, __VA_ARGS__))

#define CRPC_ORM_ENTITY(T, tname, ...)                                         \
    RTTI_DEFINE_TYPE_ATTRS(T, RTTI_ATTR("table", tname))                       \
    RTTI_DEFINE_TYPE(T, CRPC_ORM_FN_OPT(T, __VA_ARGS__))                       \
    RTTI_DEFINE_FIELD_ATTRS(T, CRPC_ORM_ENTRIES_OPT(T, __VA_ARGS__))

#endif // CRPC_ORM_ANNOTATE_HPP