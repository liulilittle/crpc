// CRPC -- deterministic identifiers for declaratively defined RPC methods.
//
// The declarative service API (`CRPC_RPC_METHOD` + `add_controller`) needs a
// service id and a method id that are stable without a hand-written table: the
// server and every client derive the same number from the same C++ type.
//
// `method_id_from<Ctl>()` produces that number at compile time. It hashes the
// compiler's textual rendering of the enclosing function template
// (`__FUNCSIG__` on MSVC, `__PRETTY_FUNCTION__` elsewhere) with FNV-1a. That
// rendering contains the fully qualified controller type, which gives three
// properties the declarative API depends on:
//
//   * It is a constant expression. The id can initialize the `inline static
//     constexpr` member the macro emits and can be used in a `static_assert`.
//
//   * It is deterministic. The same type in the same toolchain always yields the
//     same id, and distinct types yield distinct ids -- a 64-bit FNV-1a collision
//     over a set of controller names is not a practical concern.
//
//   * It is independent of the *point of use*. `__FUNCSIG__` inside
//     `method_id_from` names `method_id_from`, not its caller, so a call from
//     inside the controller's own class body (where `Ctl` is still incomplete,
//     which is exactly where the macro evaluates it) and a call from a client
//     both hash the same string. An incomplete `Ctl` is therefore fine.
//
// RTTI (`typeid(Ctl).name()`) was considered as the source string and rejected:
// it is not a constant expression, so it cannot initialize a `constexpr` member,
// and the spelling of a type name is implementation-defined. The signature macro
// provides both properties directly.
//
// Stability caveat: the hashed string is the compiler's rendering, which is not
// guaranteed to be byte-identical across compiler vendors or major versions. An
// id is stable for a given toolchain; a protocol that must interoperate across
// toolchains should pin the ids, which is why the low-level
// `add_method<Ctl, Method>` API keeps taking explicit ids.
#ifndef CRPC_RPC_METHOD_META_HPP
#define CRPC_RPC_METHOD_META_HPP

#include <crpc/config.hpp>

#if !defined(_MSC_VER) && !defined(__clang__) && !defined(__GNUC__)
#error "method_id_from needs __FUNCSIG__ or __PRETTY_FUNCTION__ to derive a deterministic id"
#endif

namespace crpc::rpc {

namespace method_meta_detail {

// FNV-1a (64-bit). Chosen because it is tiny, has no table, and is trivially
// usable in a constant expression; the quality is more than sufficient for a
// handful of type names.
inline constexpr u64 fnv1a_offset_basis = 0xcbf29ce484222325ull;
inline constexpr u64 fnv1a_prime = 0x00000100000001b3ull;

[[nodiscard]] constexpr u64 fnv1a(const char* text) noexcept {
    u64 hash = fnv1a_offset_basis;
    for (; *text != '\0'; ++text) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(*text));
        hash *= fnv1a_prime;
    }
    return hash;
}

} // namespace method_meta_detail

// A compile-time identifier derived from the controller type `Ctl`.
//
// `noexcept`, `constexpr`, and deterministic: equal types hash equal, distinct
// types (almost surely) hash distinct. See the file header for the exact
// guarantee and its stability caveat.
template <typename Ctl>
[[nodiscard]] constexpr u64 method_id_from() noexcept {
#if defined(_MSC_VER)
    constexpr const char* signature = __FUNCSIG__;
#else
    constexpr const char* signature = __PRETTY_FUNCTION__;
#endif
    return method_meta_detail::fnv1a(signature);
}

// The compile-time metadata one declared method carries: the service id derived
// from its controller type, and the method name. `name` is a non-owning pointer
// to a string literal; the declarative macro is only ever used with a literal.
struct method_meta {
    u64 service_id;
    const char* name;
};

} // namespace crpc::rpc

#endif // CRPC_RPC_METHOD_META_HPP
